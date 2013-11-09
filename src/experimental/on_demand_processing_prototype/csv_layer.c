#include "csv_layer.h"
#include "xively.h"

// xi
#include "xi_macros.h"
#include "xi_debug.h"
#include "xi_coroutine.h"
#include "xi_generator.h"
#include "xi_stated_csv_decode_value_state.h"
#include "xi_stated_sscanf.h"

#include "csv_layer_data.h"
#include "layer_api.h"
#include "http_layer_input.h"
#include "layer_helpers.h"


/** \brief holds pattern for parsing and constructing timestamp */
static const char* const CSV_TIMESTAMP_PATTERN    = "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ,";

//

inline static int csv_encode_value(
      char* buffer
    , size_t buffer_size
    , const xi_datapoint_t* p )
{
    // PRECONDITION
    assert( buffer != 0 );
    assert( buffer_size != 0 );
    assert( p != 0 );

    switch( p->value_type )
    {
        case XI_VALUE_TYPE_I32:
            return snprintf( buffer, buffer_size, "%d", p->value.i32_value );
        case XI_VALUE_TYPE_F32:
            return snprintf( buffer, buffer_size, "%f", p->value.f32_value );
        case XI_VALUE_TYPE_STR:
            return snprintf( buffer, buffer_size, "%s", p->value.str_value );
        default:
            return -1;
    }
}

typedef enum
{
    XI_STATE_INITIAL = 0,
    XI_STATE_MINUS,
    XI_STATE_NUMBER,
    XI_STATE_FLOAT,
    XI_STATE_DOT,
    XI_STATE_STRING,
    XI_STATES_NO
} xi_dfa_state_t;

typedef enum
{
    XI_CHAR_UNKNOWN = 0,
    XI_CHAR_NUMBER,
    XI_CHAR_LETTER,
    XI_CHAR_DOT,
    XI_CHAR_SPACE,
    XI_CHAR_NEWLINE,
    XI_CHAR_TAB,
    XI_CHAR_MINUS,
    XI_CHARS_NO
} xi_char_type_t;

inline static xi_char_type_t csv_classify_char( char c )
{
    switch( c )
    {
        case 13:
        case 11:
            return XI_CHAR_NEWLINE;
        case 9:
            return XI_CHAR_TAB;
        case 32:
            return XI_CHAR_SPACE;
        case 33: case 34: case 35: case 36: case 37: case 38: case 39:
        case 40: case 41: case 42: case 43: case 44:
            return XI_CHAR_UNKNOWN;
        case 45:
            return XI_CHAR_MINUS;
        case 46:
            return XI_CHAR_DOT;
        case 47:
            return XI_CHAR_UNKNOWN;
        case 48: case 49: case 50: case 51: case 52: case 53: case 54:
        case 55: case 56:
        case 57:
            return XI_CHAR_NUMBER;
        case 58: case 59: case 60: case 61: case 62: case 63:
        case 64:
            return XI_CHAR_UNKNOWN;
        case 65: case 66: case 67: case 68: case 69: case 70: case 71:
        case 72: case 73: case 74: case 75: case 76: case 77: case 78:
        case 79: case 80: case 81: case 82: case 83: case 84: case 85:
        case 86: case 87: case 88: case 89:
        case 90:
            return XI_CHAR_LETTER;
        case 91: case 92: case 93: case 94: case 95:
        case 96:
            return XI_CHAR_UNKNOWN;
        case 97: case 98: case 99: case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107: case 108: case 109: case 110:
        case 111: case 112: case 113: case 114: case 115: case 116: case 117:
        case 118: case 119: case 120: case 121:
        case 122:
            return XI_CHAR_LETTER;
        case 123:
        case 124:
        case 125:
            return XI_CHAR_UNKNOWN;
        default:
            return XI_CHAR_UNKNOWN;
    }
}

// the transition function
static const short states[][6][2] =
{
      // state initial                          // state minus                            // state number                           // state float                            // state dot                              // string
    { { XI_CHAR_UNKNOWN   , XI_STATE_STRING  }, { XI_CHAR_UNKNOWN   , XI_STATE_STRING  }, { XI_CHAR_UNKNOWN   , XI_STATE_STRING  }, { XI_CHAR_UNKNOWN   , XI_STATE_STRING  }, { XI_CHAR_UNKNOWN   , XI_STATE_STRING  }, { XI_CHAR_UNKNOWN   , XI_STATE_STRING  } },
    { { XI_CHAR_NUMBER    , XI_STATE_NUMBER  }, { XI_CHAR_NUMBER    , XI_STATE_NUMBER  }, { XI_CHAR_NUMBER    , XI_STATE_NUMBER  }, { XI_CHAR_NUMBER    , XI_STATE_FLOAT   }, { XI_CHAR_NUMBER    , XI_STATE_FLOAT   }, { XI_CHAR_NUMBER    , XI_STATE_STRING  } },
    { { XI_CHAR_LETTER    , XI_STATE_STRING  }, { XI_CHAR_LETTER    , XI_STATE_STRING  }, { XI_CHAR_LETTER    , XI_STATE_STRING  }, { XI_CHAR_LETTER    , XI_STATE_STRING  }, { XI_CHAR_LETTER    , XI_STATE_STRING  }, { XI_CHAR_LETTER    , XI_STATE_STRING  } },
    { { XI_CHAR_DOT       , XI_STATE_DOT     }, { XI_CHAR_DOT       , XI_STATE_DOT     }, { XI_CHAR_DOT       , XI_STATE_DOT     }, { XI_CHAR_DOT       , XI_STATE_STRING  }, { XI_CHAR_DOT       , XI_STATE_STRING  }, { XI_CHAR_DOT       , XI_STATE_STRING  } },
    { { XI_CHAR_SPACE     , XI_STATE_STRING  }, { XI_CHAR_SPACE     , XI_STATE_STRING  }, { XI_CHAR_SPACE     , XI_STATE_STRING  }, { XI_CHAR_SPACE     , XI_STATE_STRING  }, { XI_CHAR_SPACE     , XI_STATE_STRING  }, { XI_CHAR_SPACE     , XI_STATE_STRING  } },
    { { XI_CHAR_NEWLINE   , XI_STATE_INITIAL }, { XI_CHAR_NEWLINE   , XI_STATE_INITIAL }, { XI_CHAR_NEWLINE   , XI_STATE_INITIAL }, { XI_CHAR_NEWLINE   , XI_STATE_INITIAL }, { XI_CHAR_NEWLINE   , XI_STATE_INITIAL }, { XI_CHAR_NEWLINE   , XI_STATE_INITIAL } },
    { { XI_CHAR_TAB       , XI_STATE_STRING  }, { XI_CHAR_TAB       , XI_STATE_STRING  }, { XI_CHAR_TAB       , XI_STATE_STRING  }, { XI_CHAR_TAB       , XI_STATE_STRING  }, { XI_CHAR_TAB       , XI_STATE_STRING  }, { XI_CHAR_TAB       , XI_STATE_STRING  } },
    { { XI_CHAR_MINUS     , XI_STATE_MINUS   }, { XI_CHAR_MINUS     , XI_STATE_STRING  }, { XI_CHAR_MINUS     , XI_STATE_STRING  }, { XI_CHAR_MINUS     , XI_STATE_STRING  }, { XI_CHAR_MINUS     , XI_STATE_STRING  }, { XI_CHAR_MINUS     , XI_STATE_STRING  } }
};

// @TODO
// static const short accepting_states[] = { XI_STATE_ };

char xi_stated_csv_decode_value(
          xi_stated_csv_decode_value_state_t* st
        , const_data_descriptor_t* source
        , xi_datapoint_t* p
        , layer_hint_t hint )
{
    // unused
    XI_UNUSED( hint );

    // PRECONDITION
    assert( st != 0 );
    assert( source != 0 );
    assert( p != 0 );

    // if not the first run jump into the proper label
    if( st->state != XI_STATE_INITIAL )
    {
        goto data_ready;
    }

    // secure the output buffer
    XI_GUARD_EOS( p->value.str_value, XI_VALUE_STRING_MAX_SIZE );

    // clean the counter
    st->counter = 0;

    // check if the buffer needs more data
    if( source->curr_pos == source->real_size )
    {
        return LAYER_STATE_MORE_DATA;
    }

    char    c   = source->data_ptr[ source->curr_pos++ ];
    st->state   = XI_STATE_INITIAL;

    // main processing loop
    while( c != '\n' && c !='\0' && c!='\r' )
    {
        if( st->counter >= XI_VALUE_STRING_MAX_SIZE - 1 )
        {
            xi_set_err( XI_DATAPOINT_VALUE_BUFFER_OVERFLOW );
            return 0;
        }

        xi_char_type_t ct = csv_classify_char( c );
        st->state = states[ ct ][ st->state ][ 1 ];

        switch( st->state )
        {
            case XI_STATE_MINUS:
            case XI_STATE_NUMBER:
            case XI_STATE_FLOAT:
            case XI_STATE_DOT:
            case XI_STATE_STRING:
                p->value.str_value[ st->counter ] = c;
                break;
        }

        // this is where we shall need to jump for more data
        if( source->curr_pos == source->real_size )
        {
            // need more data
            return 0;
data_ready:
            source->curr_pos = 0; // reset the counter
        }

        //
        ++( st->counter );
        c = source->data_ptr[ ( source->curr_pos )++ ];
    }

    // set the guard
    p->value.str_value[ st->counter ] = '\0';

    // update of the state for loose states...
    switch( st->state )
    {
        case XI_STATE_MINUS:
        case XI_STATE_DOT:
        case XI_STATE_INITIAL:
            st->state = XI_STATE_STRING;
            break;
    }

    switch( st->state )
    {
        case XI_STATE_NUMBER:
            p->value.i32_value  = atoi( p->value.str_value );
            p->value_type       = XI_VALUE_TYPE_I32;
            break;
        case XI_STATE_FLOAT:
            p->value.f32_value  = ( float ) atof( p->value.str_value );
            p->value_type       = XI_VALUE_TYPE_F32;
            break;
        case XI_STATE_STRING:
        default:
            p->value_type       = XI_VALUE_TYPE_STR;
    }

    return 1;
}

/**
 * @brief csv_layer_data_generator_datapoint generates the data related to the datapoint
 * @param input
 * @param state
 * @return
 */
const void* csv_layer_data_generator_datapoint(
          const void* input
        , short* state )
{
    // we expect input to be datapoint
    const xi_datapoint_t* dp = ( xi_datapoint_t* ) input;

    ENABLE_GENERATOR();
    BEGIN_CORO( *state )

        // if there is a timestamp encode it
        if( dp->timestamp.timestamp != 0 )
        {
            xi_time_t stamp = dp->timestamp.timestamp;
            struct xi_tm* gmtinfo = xi_gmtime( &stamp );

            static char buffer[ 32 ] = { '\0' };

            snprintf( buffer, 32
                , CSV_TIMESTAMP_PATTERN
                , gmtinfo->tm_year + 1900
                , gmtinfo->tm_mon + 1
                , gmtinfo->tm_mday
                , gmtinfo->tm_hour
                , gmtinfo->tm_min
                , gmtinfo->tm_sec
                , ( int ) dp->timestamp.micro );

            gen_ptr_text( *state, buffer );
        }

        // value
        {
            static char buffer[ 32 ] = { '\0' };
            csv_encode_value( buffer, sizeof( buffer ), dp );
            gen_ptr_text( *state, buffer );
        }

        // end of line
        gen_static_text_and_exit( *state, "\n" );

    END_CORO()
}

/**
 * @brief csv_layer_data_generator_datastream_update
 * @param input
 * @param state
 * @return
 */
const void* csv_layer_data_generator_datastream_update(
          const void* input
        , short* state )
{
    XI_UNUSED( input );

    ENABLE_GENERATOR();
    BEGIN_CORO( *state )

        gen_static_text( *state, "sub_test0" );
        gen_static_text( *state, "sub_test1" );
        gen_static_text( *state, "sub_test2" );
        gen_static_text( *state, "sub_test3" );

        call_sub_gen( *state, input, csv_layer_data_generator_datapoint );

        gen_static_text( *state, "sub_test4" );
        gen_static_text_and_exit( *state, "sub_test5" );

    END_CORO()
}


/**
 * \brief  see the layer_interface for details
 */
const void* csv_layer_data_generator_datastream_get(
          const void* input
        , short* state )
{
    XI_UNUSED( input );

    ENABLE_GENERATOR();
    BEGIN_CORO( *state )

        gen_static_text( *state, "test0" );
        gen_static_text( *state, "test1" );
        gen_static_text( *state, "test2" );

        call_sub_gen( *state, input, csv_layer_data_generator_datastream_update );

        gen_static_text( *state, "test3" );
        gen_static_text( *state, "test4" );
        gen_static_text_and_exit( *state, "test5" );

    END_CORO()
}


/**
 * @brief csv_layer_parse_datastream helper function that parses the one level of the data which is the datastream itself
 *        this function suppose to parse the timestamp and the value and save it within the proper datastream field
 *
 * @param context
 * @param data
 * @param hint
 * @return
 */
layer_state_t csv_layer_parse_datastream(
        csv_layer_data_t* csv_layer_data
      , const_data_descriptor_t* data
      , const layer_hint_t hint
      , xi_datapoint_t* dp )
{
    XI_UNUSED( hint );
    XI_UNUSED( dp );

    // some tmp variables
    char ret_state                      = 0;

    BEGIN_CORO( csv_layer_data->datapoint_decode_state )

    // parse the timestamp
    {
        static struct xi_tm gmtinfo;
        memset( &gmtinfo, 0, sizeof( struct xi_tm ) );

        do
        {
            const const_data_descriptor_t pattern   = { CSV_TIMESTAMP_PATTERN, strlen( CSV_TIMESTAMP_PATTERN ), strlen( CSV_TIMESTAMP_PATTERN ), 0 };
            void* pv[]                              = {
                ( void* ) &( gmtinfo.tm_year )
                , ( void* ) &( gmtinfo.tm_mon )
                , ( void* ) &( gmtinfo.tm_mday )
                , ( void* ) &( gmtinfo.tm_hour )
                , ( void* ) &( gmtinfo.tm_min )
                , ( void* ) &( gmtinfo.tm_sec )
                , ( void* ) &( dp->timestamp.micro ) };

            ret_state = xi_stated_sscanf( &( csv_layer_data->stated_sscanf_state ), &pattern, data, pv );

            if( ret_state == 0 )
            {
                // need more data
                YIELD( csv_layer_data->datapoint_decode_state, LAYER_STATE_MORE_DATA )
                continue;
            }

        } while( ret_state == 0 );

        // test the result
        if( ret_state == -1 )
        {
            EXIT( csv_layer_data->datapoint_decode_state, LAYER_STATE_ERROR );
        }

        // here it's safe to convert the gmtinfo to timestamp
        gmtinfo.tm_year            -= 1900;
        gmtinfo.tm_mon             -= 1;
        dp->timestamp.timestamp     = xi_mktime( &gmtinfo );
    }


    // parse the value
    {
        do
        {
            ret_state = xi_stated_csv_decode_value( &( csv_layer_data->csv_decode_value_state ), data, dp, LAYER_HINT_NONE );

            if( ret_state == 0 )
            {
                YIELD( csv_layer_data->datapoint_decode_state, LAYER_STATE_MORE_DATA )
                continue;
            }

        } while( ret_state == 0 );
    }

    // exit the function
    EXIT( csv_layer_data->datapoint_decode_state, ( ret_state == -1 ? LAYER_STATE_ERROR : LAYER_STATE_OK ) );

    END_CORO()

    return LAYER_STATE_OK;
}

/**
 * @brief csv_layer_parse_feed
 * @param csv_layer_data
 * @param data
 * @param hint
 * @param dp
 * @return
 */
layer_state_t csv_layer_parse_feed(
        csv_layer_data_t* csv_layer_data
      , const_data_descriptor_t* data
      , const layer_hint_t hint
      , xi_feed_t* dp )
{
    XI_UNUSED( hint );
    XI_UNUSED( dp );

    // some tmp variables
    char sscanf_state                      = 0;
    layer_state_t state                    = LAYER_STATE_OK;

    BEGIN_CORO( csv_layer_data->feed_decode_state )

    // clear the count of datastreams that has been read so far
    dp->datastream_count = 0;

    // loop over datastreams
    do
    {
        // we are expecting the patttern with datapoint id
        {
            do
            {
                const char status_pattern[]       = "%" XI_STR( XI_MAX_DATASTREAM_NAME ) "C,";
                const const_data_descriptor_t v   = { status_pattern, sizeof( status_pattern ) - 1, sizeof( status_pattern ) - 1, 0 };
                void* pv[]                        = { ( void* ) dp->datastreams[ dp->datastream_count ].datastream_id };

                sscanf_state = xi_stated_sscanf(
                              &( csv_layer_data->stated_sscanf_state )
                            , ( const_data_descriptor_t* ) &v
                            , ( const_data_descriptor_t* ) data
                            , pv );

                if( sscanf_state == 0 )
                {
                    YIELD( csv_layer_data->feed_decode_state, LAYER_STATE_MORE_DATA );
                    sscanf_state = 0;
                    continue;
                }
                else if( sscanf_state == -1 )
                {
                    EXIT( csv_layer_data->feed_decode_state, LAYER_STATE_ERROR );
                }

            } while( sscanf_state == 0 );
        }

        // after parsing the datastream name it's time for the datastream itself
        {
            //
            do
            {
                state = csv_layer_parse_datastream( csv_layer_data, data, hint, &( dp->datastreams[ dp->datastream_count ].datapoints[ 0 ] ) );

                if( state == LAYER_STATE_MORE_DATA )
                {
                    YIELD( csv_layer_data->feed_decode_state, LAYER_STATE_MORE_DATA );
                    state = LAYER_STATE_MORE_DATA;
                    continue;
                }
                else if( state == LAYER_STATE_ERROR )
                {
                    EXIT( csv_layer_data->feed_decode_state, LAYER_STATE_ERROR );
                }

            } while( state == LAYER_STATE_MORE_DATA );

            // update the counter values
            dp->datastreams[ dp->datastream_count ].datapoint_count     = 1;
            dp->datastream_count                                       += 1;
        }

    } while( hint == LAYER_HINT_MORE_DATA ); // stop condition

    EXIT( csv_layer_data->feed_decode_state, LAYER_STATE_OK );

    END_CORO()

    return LAYER_STATE_OK;
}

layer_state_t csv_layer_data_ready(
      layer_connectivity_t* context
    , const void* data
    , const layer_hint_t hint )
{
    XI_UNUSED( context );
    XI_UNUSED( data );
    XI_UNUSED( hint );

    // unpack the layer data
    csv_layer_data_t* csv_layer_data = ( csv_layer_data_t* ) context->self->user_data;

    layer_state_t state = LAYER_STATE_OK;

    //
    switch( csv_layer_data->http_layer_input->query_type )
    {
        case HTTP_LAYER_INPUT_DATASTREAM_GET:
                return csv_layer_parse_datastream( csv_layer_data, data, hint, csv_layer_data->http_layer_input->http_layer_data_u.xi_get_datastream.value );
            break;
        default:
            break;
    }

    return LAYER_STATE_OK;

    // END_CORO()

    return LAYER_STATE_OK;
}

layer_state_t csv_layer_on_data_ready(
      layer_connectivity_t* context
    , const void* data
    , const layer_hint_t hint )
{
    XI_UNUSED( context );
    XI_UNUSED( data );
    XI_UNUSED( hint );

    // unpack the data, changing the constiness to avoid copying cause
    // these layers shares the same data and the generator suppose to be the only
    // field that set is required
    http_layer_input_t* http_layer_input = ( http_layer_input_t* ) ( data );

    // store the layer input in custom data will need that later
    // during the response parsing
    ( ( csv_layer_data_t* ) context->self->user_data )->http_layer_input = ( void* ) http_layer_input;

    switch( http_layer_input->query_type )
    {
        case HTTP_LAYER_INPUT_DATASTREAM_GET:
        {
            http_layer_input->payload_generator = 0;
            return CALL_ON_PREV_DATA_READY( context->self, ( void* ) http_layer_input, hint );
        }
        default:
            return LAYER_STATE_ERROR;
    };

    return LAYER_STATE_ERROR;
}

layer_state_t csv_layer_close(
    layer_connectivity_t* context )
{
    XI_UNUSED( context );

    return LAYER_STATE_OK;
}

layer_state_t csv_layer_on_close(
    layer_connectivity_t* context )
{
    XI_UNUSED( context );

    return LAYER_STATE_OK;
}
