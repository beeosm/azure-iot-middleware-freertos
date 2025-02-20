/* Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License. */

#include "e2e_device_process_commands.h"

#include <time.h>

#include "azure_iot_http.h"
#include "azure_iot_transport_interface.h"
#include "azure_iot_json_writer.h"
#include "azure_iot_jws.h"
#include "azure_iot_hub_client_properties.h"
#include "azure/core/az_json.h"
#include "azure/core/az_span.h"

#define e2etestMESSAGE_BUFFER_SIZE                         ( 10240 )

#define e2etestPROCESS_LOOP_WAIT_TIMEOUT_IN_MSEC           ( 2 * 1000 )

#define e2etestE2E_TEST_SUCCESS                            ( 0 )
#define e2etestE2E_TEST_FAILED                             ( 1 )
#define e2etestE2E_TEST_NOT_FOUND                          ( 2 )

#define e2etestE2E_HMAC_MAX_SIZE                           ( 32 )

#define e2etestMETHOD_KEY                                  "method"
#define e2etestPAYLOAD_KEY                                 "payload"
#define e2etestPROPERTIES_KEY                              "properties"
#define e2etestID_SCOPE_KEY                                "id_scope"
#define e2etestREGISTRATION_ID_KEY                         "registration_id"
#define e2etestSYMMETRIC_KEY                               "symmetric_key"
#define e2etestSERVICE_ENDPOINT_KEY                        "service_endpoint"

#define e2etestE2E_TEST_ECHO_COMMAND                       "echo"
#define e2etestE2E_TEST_EXIT_COMMAND                       "exit"
#define e2etestE2E_TEST_SEND_INITIAL_ADU_STATE_COMMAND     "send_init_adu_state"
#define e2etestE2E_TEST_GET_ADU_TWIN_PROPERTIES_COMMAND    "get_adu_twin"
#define e2etestE2E_TEST_APPLY_ADU_UPDATE_COMMAND           "apply_update"
#define e2etestE2E_TEST_VERIFY_ADU_FINAL_STATE_COMMAND     "verify_final_state"
#define e2etestE2E_TEST_CONNECTED_MESSAGE                  "\"CONNECTED\""
#define e2etestE2E_TEST_ADU_PAYLOAD_RECEIVED               "\"ADU-RECEIVED\""
#define e2etestE2E_TEST_WAITING_FOR_ADU                    "\"Waiting for ADU\""

#define RETURN_IF_FAILED( e, msg )                    \
    if( e != e2etestE2E_TEST_SUCCESS )                \
    {                                                 \
        printf( "[%s][%d]%s : error code: 0x%0x\r\n", \
                __FILE__, __LINE__, msg, e );         \
        return ( e );                                 \
    }
/*-----------------------------------------------------------*/

struct E2E_TEST_COMMAND_STRUCT;
typedef uint32_t ( * EXECUTE_FN ) ( struct E2E_TEST_COMMAND_STRUCT * pxCMD,
                                    AzureIoTHubClient_t * pxAzureIoTHubClient,
                                    AzureIoTADUClient_t * pxAzureIoTAduClient );

typedef struct E2E_TEST_COMMAND_STRUCT
{
    EXECUTE_FN xExecute;
    const uint8_t * pulReceivedData;
    uint32_t ulReceivedDataLength;
} E2E_TEST_COMMAND;

typedef E2E_TEST_COMMAND * E2E_TEST_COMMAND_HANDLE;
/*-----------------------------------------------------------*/

static uint8_t * ucC2DCommandData = NULL;
static uint32_t ulC2DCommandDataLength = 0;
static AzureIoTHubClientPropertiesResponse_t * pxTwinMessage = NULL;
static AzureIoTADUUpdateRequest_t xAzureIoTAduUpdateRequest;
static uint8_t ucMethodNameBuffer[ e2etestMESSAGE_BUFFER_SIZE ];
static uint8_t ucScratchBuffer[ e2etestMESSAGE_BUFFER_SIZE ];
static uint8_t ucScratchBuffer2[ e2etestMESSAGE_BUFFER_SIZE ];
static uint8_t ucMessageBuffer[ e2etestMESSAGE_BUFFER_SIZE ];
static const uint8_t ucStatusOKTelemetry[] = "{\"status\": \"OK\"}";
static uint32_t ulContinueProcessingCMD = 1;
static uint8_t ucSharedBuffer[ e2etestMESSAGE_BUFFER_SIZE ];
static uint16_t usReceivedTelemetryPubackID;
/*-----------------------------------------------------------*/

/* ADU Values */
#define e2eADU_DEVICE_MANUFACTURER    "PC"
#define e2eADU_DEVICE_MODEL           "Linux-E2E"
#define e2eADU_UPDATE_PROVIDER        "ADU-E2E-Tests"
#define e2eADU_UPDATE_NAME            "Linux-E2E-Update"
#define e2eADU_UPDATE_VERSION         "1.0"
#define e2eADU_UPDATE_VERSION_NEW     "1.1"
#define e2eADU_UPDATE_ID              "{\"provider\":\"" e2eADU_UPDATE_PROVIDER "\",\"name\":\"" e2eADU_UPDATE_NAME "\",\"version\":\"" e2eADU_UPDATE_VERSION "\"}"

static AzureIoTADUClientDeviceProperties_t xADUDeviceProperties =
{
    .ucManufacturer                           = ( const uint8_t * ) e2eADU_DEVICE_MANUFACTURER,
    .ulManufacturerLength                     = sizeof( e2eADU_DEVICE_MANUFACTURER ) - 1,
    .ucModel                                  = ( const uint8_t * ) e2eADU_DEVICE_MODEL,
    .ulModelLength                            = sizeof( e2eADU_DEVICE_MODEL ) - 1,
    .ucCurrentUpdateId                        = ( const uint8_t * ) e2eADU_UPDATE_ID,
    .ulCurrentUpdateIdLength                  = sizeof( e2eADU_UPDATE_ID ) - 1,
    .ucDeliveryOptimizationAgentVersion       = NULL,
    .ulDeliveryOptimizationAgentVersionLength = 0
};

static AzureIoTHTTP_t xHTTPClient;
static bool xAduWasReceived = false;
static uint32_t ulReceivedTwinVersion;
static uint8_t ucAduManifestVerificationBuffer[ azureiotjwsSCRATCH_BUFFER_SIZE ];


/* ADU.200702.R */
static uint8_t ucAzureIoTADURootKeyId200702[ 13 ] = "ADU.200702.R";
static uint8_t ucAzureIoTADURootKeyN200702[ 385 ]
    =
    {
    0x00, 0xd5, 0x42, 0x2e, 0xaf, 0x11, 0x54, 0xa3, 0x50, 0x65, 0x87, 0xa2, 0x4d, 0x5b, 0xba,
    0x1a, 0xfb, 0xa9, 0x32, 0xdf, 0xe9, 0x99, 0x5f, 0x05, 0x45, 0xc8, 0xaf, 0xbd, 0x35, 0x1d,
    0x89, 0xe8, 0x27, 0x27, 0x58, 0xa3, 0xa8, 0xee, 0xc5, 0xc5, 0x1e, 0x4f, 0xf7, 0x92, 0xa6,
    0x12, 0x06, 0x7d, 0x3d, 0x7d, 0xb0, 0x07, 0xf6, 0x2c, 0x7f, 0xde, 0x6d, 0x2a, 0xf5, 0xbc,
    0x49, 0xbc, 0x15, 0xef, 0xf0, 0x81, 0xcb, 0x3f, 0x88, 0x4f, 0x27, 0x1d, 0x88, 0x71, 0x28,
    0x60, 0x08, 0xb6, 0x19, 0xd2, 0xd2, 0x39, 0xd0, 0x05, 0x1f, 0x3c, 0x76, 0x86, 0x71, 0xbb,
    0x59, 0x58, 0xbc, 0xb1, 0x88, 0x7b, 0xab, 0x56, 0x28, 0xbf, 0x31, 0x73, 0x44, 0x32, 0x10,
    0xfd, 0x3d, 0xd3, 0x96, 0x5c, 0xff, 0x4e, 0x5c, 0xb3, 0x6b, 0xff, 0x8b, 0x84, 0x9b, 0x8b,
    0x80, 0xb8, 0x49, 0xd0, 0x7d, 0xfa, 0xd6, 0x40, 0x58, 0x76, 0x4d, 0xc0, 0x72, 0x27, 0x75,
    0xcb, 0x9a, 0x2f, 0x9b, 0xb4, 0x9f, 0x0f, 0x25, 0xf1, 0x1c, 0xc5, 0x1b, 0x0b, 0x5a, 0x30,
    0x7d, 0x2f, 0xb8, 0xef, 0xa7, 0x26, 0x58, 0x53, 0xaf, 0xd5, 0x1d, 0x55, 0x01, 0x51, 0x0d,
    0xe9, 0x1b, 0xa2, 0x0f, 0x3f, 0xd7, 0xe9, 0x1d, 0x20, 0x41, 0xa6, 0xe6, 0x14, 0x0a, 0xae,
    0xfe, 0xf2, 0x1c, 0x2a, 0xd6, 0xe4, 0x04, 0x7b, 0xf6, 0x14, 0x7e, 0xec, 0x0f, 0x97, 0x83,
    0xfa, 0x58, 0xfa, 0x81, 0x36, 0x21, 0xb9, 0xa3, 0x2b, 0xfa, 0xd9, 0x61, 0x0b, 0x1a, 0x94,
    0xf7, 0xc1, 0xbe, 0x7f, 0x40, 0x14, 0x4a, 0xc9, 0xfa, 0x35, 0x7f, 0xef, 0x66, 0x70, 0x00,
    0xb1, 0xfd, 0xdb, 0xd7, 0x61, 0x0d, 0x3b, 0x58, 0x74, 0x67, 0x94, 0x89, 0x75, 0x76, 0x96,
    0x7c, 0x91, 0x87, 0xd2, 0x8e, 0x11, 0x97, 0xee, 0x7b, 0x87, 0x6c, 0x9a, 0x2f, 0x45, 0xd8,
    0x65, 0x3f, 0x52, 0x70, 0x98, 0x2a, 0xcb, 0xc8, 0x04, 0x63, 0xf5, 0xc9, 0x47, 0xcf, 0x70,
    0xf4, 0xed, 0x64, 0xa7, 0x74, 0xa5, 0x23, 0x8f, 0xb6, 0xed, 0xf7, 0x1c, 0xd3, 0xb0, 0x1c,
    0x64, 0x57, 0x12, 0x5a, 0xa9, 0x81, 0x84, 0x1f, 0xa0, 0xe7, 0x50, 0x19, 0x96, 0xb4, 0x82,
    0xb1, 0xac, 0x48, 0xe3, 0xe1, 0x32, 0x82, 0xcb, 0x40, 0x1f, 0xac, 0xc4, 0x59, 0xbc, 0x10,
    0x34, 0x51, 0x82, 0xf9, 0x28, 0x8d, 0xa8, 0x1e, 0x9b, 0xf5, 0x79, 0x45, 0x75, 0xb2, 0xdc,
    0x9a, 0x11, 0x43, 0x08, 0xbe, 0x61, 0xcc, 0x9a, 0xc4, 0xcb, 0x77, 0x36, 0xff, 0x83, 0xdd,
    0xa8, 0x71, 0x4f, 0x51, 0x8e, 0x0e, 0x7b, 0x4d, 0xfa, 0x79, 0x98, 0x8d, 0xbe, 0xfc, 0x82,
    0x7e, 0x40, 0x48, 0xa9, 0x12, 0x01, 0xa8, 0xd9, 0x7e, 0xf3, 0xa5, 0x1b, 0xf1, 0xfb, 0x90,
    0x77, 0x3e, 0x40, 0x87, 0x18, 0xc9, 0xab, 0xd9, 0xf7, 0x79
    };
static uint8_t ucAzureIoTADURootKeyE200702[ 3 ] = { 0x01, 0x00, 0x01 };

/* ADU.200703.R */
static uint8_t ucAzureIoTADURootKeyId200703[ 13 ] = "ADU.200703.R";
static uint8_t ucAzureIoTADURootKeyN200703[ 385 ] =
{
    0x00, 0xb2, 0xa3, 0xb2, 0x74, 0x16, 0xfa, 0xbb, 0x20, 0xf9, 0x52, 0x76, 0xe6, 0x27, 0x3e,
    0x80, 0x41, 0xc6, 0xfe, 0xcf, 0x30, 0xf9, 0xc8, 0x96, 0xf5, 0x59, 0x0a, 0xaa, 0x81, 0xe7,
    0x51, 0x83, 0x8a, 0xc4, 0xf5, 0x17, 0x3a, 0x2f, 0x2a, 0xe6, 0x57, 0xd4, 0x71, 0xce, 0x8a,
    0x3d, 0xef, 0x9a, 0x55, 0x76, 0x3e, 0x99, 0xe2, 0xc2, 0xae, 0x4c, 0xee, 0x2d, 0xb8, 0x78,
    0xf5, 0xa2, 0x4e, 0x28, 0xf2, 0x9c, 0x4e, 0x39, 0x65, 0xbc, 0xec, 0xe4, 0x0d, 0xe5, 0xe3,
    0x38, 0xa8, 0x59, 0xab, 0x08, 0xa4, 0x1b, 0xb4, 0xf4, 0xa0, 0x52, 0xa3, 0x38, 0xb3, 0x46,
    0x21, 0x13, 0xcc, 0x3c, 0x68, 0x06, 0xde, 0xfe, 0x00, 0xa6, 0x92, 0x6e, 0xde, 0x4c, 0x47,
    0x10, 0xd6, 0x1c, 0x9c, 0x24, 0xf5, 0xcd, 0x70, 0xe1, 0xf5, 0x6a, 0x7c, 0x68, 0x13, 0x1d,
    0xe1, 0xc5, 0xf6, 0xa8, 0x4f, 0x21, 0x9f, 0x86, 0x7c, 0x44, 0xc5, 0x8a, 0x99, 0x1c, 0xc5,
    0xd3, 0x06, 0x9b, 0x5a, 0x71, 0x9d, 0x09, 0x1c, 0xc3, 0x64, 0x31, 0x6a, 0xc5, 0x17, 0x95,
    0x1d, 0x5d, 0x2a, 0xf1, 0x55, 0xc7, 0x66, 0xd4, 0xe8, 0xf5, 0xd9, 0xa9, 0x5b, 0x8c, 0xa2,
    0x6c, 0x62, 0x60, 0x05, 0x37, 0xd7, 0x32, 0xb0, 0x73, 0xcb, 0xf7, 0x4b, 0x36, 0x27, 0x24,
    0x21, 0x8c, 0x38, 0x0a, 0xb8, 0x18, 0xfe, 0xf5, 0x15, 0x60, 0x35, 0x8b, 0x35, 0xef, 0x1e,
    0x0f, 0x88, 0xa6, 0x13, 0x8d, 0x7b, 0x7d, 0xef, 0xb3, 0xe7, 0xb0, 0xc9, 0xa6, 0x1c, 0x70,
    0x7b, 0xcc, 0xf2, 0x29, 0x8b, 0x87, 0xf7, 0xbd, 0x9d, 0xb6, 0x88, 0x6f, 0xac, 0x73, 0xff,
    0x72, 0xf2, 0xef, 0x48, 0x27, 0x96, 0x72, 0x86, 0x06, 0xa2, 0x5c, 0xe3, 0x7d, 0xce, 0xb0,
    0x9e, 0xe5, 0xc2, 0xd9, 0x4e, 0xc4, 0xf3, 0x7f, 0x78, 0x07, 0x4b, 0x65, 0x88, 0x45, 0x0c,
    0x11, 0xe5, 0x96, 0x56, 0x34, 0x88, 0x2d, 0x16, 0x0e, 0x59, 0x42, 0xd2, 0xf7, 0xd9, 0xed,
    0x1d, 0xed, 0xc9, 0x37, 0x77, 0x44, 0x7e, 0xe3, 0x84, 0x36, 0x9f, 0x58, 0x13, 0xef, 0x6f,
    0xe4, 0xc3, 0x44, 0xd4, 0x77, 0x06, 0x8a, 0xcf, 0x5b, 0xc8, 0x80, 0x1c, 0xa2, 0x98, 0x65,
    0x0b, 0x35, 0xdc, 0x73, 0xc8, 0x69, 0xd0, 0x5e, 0xe8, 0x25, 0x43, 0x9e, 0xf6, 0xd8, 0xab,
    0x05, 0xaf, 0x51, 0x29, 0x23, 0x55, 0x40, 0x58, 0x10, 0xea, 0xb8, 0xe2, 0xcd, 0x5d, 0x79,
    0xcc, 0xec, 0xdf, 0xb4, 0x5b, 0x98, 0xc7, 0xfa, 0xe3, 0xd2, 0x6c, 0x26, 0xce, 0x2e, 0x2c,
    0x56, 0xe0, 0xcf, 0x8d, 0xee, 0xfd, 0x93, 0x12, 0x2f, 0x00, 0x49, 0x8d, 0x1c, 0x82, 0x38,
    0x56, 0xa6, 0x5d, 0x79, 0x44, 0x4a, 0x1a, 0xf3, 0xdc, 0x16, 0x10, 0xb3, 0xc1, 0x2d, 0x27,
    0x11, 0xfe, 0x1b, 0x98, 0x05, 0xe4, 0xa3, 0x60, 0x31, 0x99
};
static uint8_t ucAzureIoTADURootKeyE200703[ 3 ] = { 0x01, 0x00, 0x01 };

static AzureIoTJWS_RootKey_t xADURootKey[] =
{
    /*Put Root key 03 first since we know 02 is used in these ut (makes sure we cycle through the array) */
    {
        .pucRootKeyId = ucAzureIoTADURootKeyId200703,
        .ulRootKeyIdLength = sizeof( ucAzureIoTADURootKeyId200703 ) - 1,
        .pucRootKeyN = ucAzureIoTADURootKeyN200703,
        .ulRootKeyNLength = sizeof( ucAzureIoTADURootKeyN200703 ),
        .pucRootKeyExponent = ucAzureIoTADURootKeyE200703,
        .ulRootKeyExponentLength = sizeof( ucAzureIoTADURootKeyE200703 )
    },
    {
        .pucRootKeyId = ucAzureIoTADURootKeyId200702,
        .ulRootKeyIdLength = sizeof( ucAzureIoTADURootKeyId200702 ) - 1,
        .pucRootKeyN = ucAzureIoTADURootKeyN200702,
        .ulRootKeyNLength = sizeof( ucAzureIoTADURootKeyN200702 ),
        .pucRootKeyExponent = ucAzureIoTADURootKeyE200702,
        .ulRootKeyExponentLength = sizeof( ucAzureIoTADURootKeyE200702 )
    }
};

/*-----------------------------------------------------------*/

/**
 * Telemetry PUBACK callback
 *
 **/
void prvTelemetryPubackCallback( uint16_t usPacketID )
{
    AZLogInfo( ( "Puback received for packet id in callback: 0x%08x", usPacketID ) );
    usReceivedTelemetryPubackID = usPacketID;
}

/**
 * Telemetry PUBACK callback
 *
 **/
static uint32_t prvWaitForPuback( AzureIoTHubClient_t * pxAzureIoTHubClient,
                                  uint16_t usSentTelemetryPublishID )
{
    uint32_t xResult;

    AzureIoTHubClient_ProcessLoop( pxAzureIoTHubClient, e2etestPROCESS_LOOP_WAIT_TIMEOUT_IN_MSEC );

    LogInfo( ( "Checking PUBACK packet id: rec[%d] = sent[%d] ", usReceivedTelemetryPubackID, usSentTelemetryPublishID ) );

    if( usReceivedTelemetryPubackID == usSentTelemetryPublishID )
    {
        usReceivedTelemetryPubackID = 0; /* Reset received to 0 */
        xResult = 0;
    }
    else
    {
        xResult = 1;
    }

    return xResult;
}

/*-----------------------------------------------------------*/

/**
 * Free Twin data
 *
 **/
static void prvFreeTwinData( AzureIoTHubClientPropertiesResponse_t * pxTwinData )
{
    if( pxTwinData )
    {
        vPortFree( ( void * ) pxTwinData->pvMessagePayload );
        vPortFree( ( void * ) pxTwinData );
    }
}
/*-----------------------------------------------------------*/

/**
 * Free Method data
 *
 **/
static void prvFreeMethodData( AzureIoTHubClientCommandRequest_t * pxMethodData )
{
    if( pxMethodData )
    {
        vPortFree( ( void * ) pxMethodData->pvMessagePayload );
        vPortFree( ( void * ) pxMethodData->pucCommandName );
        vPortFree( ( void * ) pxMethodData->pucRequestID );
        vPortFree( ( void * ) pxMethodData );
    }
}
/*-----------------------------------------------------------*/

/**
 * Scan forward till key is not found
 *
 **/
static uint32_t prvGetJsonValueForKey( az_json_reader * pxState,
                                       const char * pcKey,
                                       az_span * pxValue )
{
    uint32_t ulStatus = e2etestE2E_TEST_NOT_FOUND;
    az_span xKeySpan = az_span_create( ( uint8_t * ) pcKey, strlen( pcKey ) );
    int32_t lLength;

    while( az_result_succeeded( az_json_reader_next_token( pxState ) ) )
    {
        if( az_json_token_is_text_equal( &( pxState->token ), xKeySpan ) )
        {
            if( az_result_failed( az_json_reader_next_token( pxState ) ) )
            {
                ulStatus = e2etestE2E_TEST_FAILED;
                LogError( ( "Failed next token, error code : %d", ulStatus ) );
                break;
            }

            if( az_result_failed( az_json_token_get_string( &pxState->token,
                                                            ( char * ) az_span_ptr( *pxValue ),
                                                            az_span_size( *pxValue ), &lLength ) ) )
            {
                ulStatus = e2etestE2E_TEST_FAILED;
                LogError( ( "Failed get string value, error code : %d", ulStatus ) );
                break;
            }

            *pxValue = az_span_create( az_span_ptr( *pxValue ), lLength );
            ulStatus = e2etestE2E_TEST_SUCCESS;
            break;
        }
    }

    return ulStatus;
}

/*-----------------------------------------------------------*/

static void prvSkipPropertyAndValue( AzureIoTJSONReader_t * pxReader )
{
    AzureIoTResult_t xResult;

    xResult = AzureIoTJSONReader_NextToken( pxReader );
    configASSERT( xResult == eAzureIoTSuccess );

    xResult = AzureIoTJSONReader_SkipChildren( pxReader );
    configASSERT( xResult == eAzureIoTSuccess );

    xResult = AzureIoTJSONReader_NextToken( pxReader );
    configASSERT( xResult == eAzureIoTSuccess );
}

/*-----------------------------------------------------------*/

/**
 * Fully parses the twin document for the ADU subcomponent.
 */
static uint32_t prvParseADUTwin( AzureIoTHubClient_t * pxAzureIoTHubClient,
                                 AzureIoTADUClient_t * pxAzureIoTAduClient,
                                 AzureIoTHubClientPropertiesResponse_t * pxPropertiesResponse )
{
    uint32_t ulStatus = e2etestE2E_TEST_SUCCESS;

    AzureIoTResult_t xAzIoTResult;
    AzureIoTJSONReader_t xJsonReader;
    const uint8_t * pucComponentName = NULL;
    uint32_t ulComponentNameLength = 0;
    uint32_t ulPropertyVersion;

    if( pxPropertiesResponse->xMessageType != eAzureIoTHubPropertiesReportedResponseMessage )
    {
        uint32_t ulWritablePropertyResponseBufferLength;
        uint32_t * pulWritablePropertyResponseBufferLength = &ulWritablePropertyResponseBufferLength;

        LogInfo( ( "Writable properties received: %.*s\r\n",
                   pxPropertiesResponse->ulPayloadLength, ( char * ) pxPropertiesResponse->pvMessagePayload ) );

        xAzIoTResult = AzureIoTJSONReader_Init( &xJsonReader, pxPropertiesResponse->pvMessagePayload, pxPropertiesResponse->ulPayloadLength );

        if( xAzIoTResult != eAzureIoTSuccess )
        {
            LogError( ( "AzureIoTJSONReader_Init failed: result 0x%08x", xAzIoTResult ) );
            *pulWritablePropertyResponseBufferLength = 0;
            return e2etestE2E_TEST_FAILED;
        }

        xAzIoTResult = AzureIoTHubClientProperties_GetPropertiesVersion( pxAzureIoTHubClient, &xJsonReader, pxPropertiesResponse->xMessageType, &ulReceivedTwinVersion );

        if( xAzIoTResult != eAzureIoTSuccess )
        {
            LogError( ( "AzureIoTHubClientProperties_GetPropertiesVersion failed: result 0x%08x", xAzIoTResult ) );
            *pulWritablePropertyResponseBufferLength = 0;
            return e2etestE2E_TEST_FAILED;
        }

        xAzIoTResult = AzureIoTJSONReader_Init( &xJsonReader, pxPropertiesResponse->pvMessagePayload, pxPropertiesResponse->ulPayloadLength );

        if( xAzIoTResult != eAzureIoTSuccess )
        {
            LogError( ( "AzureIoTJSONReader_Init failed: result 0x%08x", xAzIoTResult ) );
            *pulWritablePropertyResponseBufferLength = 0;
            return e2etestE2E_TEST_FAILED;
        }

        /**
         * If the PnP component is for Azure Device Update, function
         * AzureIoTADUClient_SendResponse shall be used to publish back the
         * response for the ADU writable properties.
         * Thus, to prevent this callback to publish a response in duplicate,
         * pulWritablePropertyResponseBufferLength must be set to zero.
         */
        *pulWritablePropertyResponseBufferLength = 0;

        while( ( xAzIoTResult = AzureIoTHubClientProperties_GetNextComponentProperty( pxAzureIoTHubClient, &xJsonReader,
                                                                                      pxPropertiesResponse->xMessageType, eAzureIoTHubClientPropertyWritable,
                                                                                      &pucComponentName, &ulComponentNameLength ) ) == eAzureIoTSuccess )
        {
            LogInfo( ( "Properties component name: %.*s", ulComponentNameLength, pucComponentName ) );

            if( AzureIoTADUClient_IsADUComponent( pxAzureIoTAduClient, pucComponentName, ulComponentNameLength ) )
            {
                AzureIoTADURequestDecision_t xRequestDecision;

                xAzIoTResult = AzureIoTADUClient_ParseRequest(
                    pxAzureIoTAduClient,
                    &xJsonReader,
                    &xAzureIoTAduUpdateRequest );
            }
            else
            {
                LogInfo( ( "Component not ADU: %.*s", ulComponentNameLength, pucComponentName ) );
                prvSkipPropertyAndValue( &xJsonReader );
            }
        }
    }

    return ulStatus;
}

/*-----------------------------------------------------------*/

/**
 * This checks if the ADU subcomponent exists in the twin.
 * We have to wait for the update to be deployed to the device before we can
 * start the tests. Once we receive a PATCH update with the subcomponent
 * successfully parsed, we move on to start the tests where we will do a full
 * twin GET and parse the whole twin (void of possible NULLs for values in a
 * twin PATCH update).
 */
static uint32_t prvCheckTwinForADU( AzureIoTHubClient_t * pxAzureIoTHubClient,
                                    AzureIoTADUClient_t * pxAzureIoTAduClient,
                                    AzureIoTHubClientPropertiesResponse_t * pxPropertiesResponse )
{
    uint32_t ulStatus = e2etestE2E_TEST_SUCCESS;

    AzureIoTResult_t xAzIoTResult;
    AzureIoTJSONReader_t xJsonReader;
    const uint8_t * pucComponentName = NULL;
    uint32_t ulComponentNameLength = 0;
    uint32_t ulPropertyVersion;

    if( pxPropertiesResponse->xMessageType != eAzureIoTHubPropertiesReportedResponseMessage )
    {
        uint32_t ulWritablePropertyResponseBufferLength;
        uint32_t * pulWritablePropertyResponseBufferLength = &ulWritablePropertyResponseBufferLength;

        LogInfo( ( "Writable properties received: %.*s\r\n",
                   pxPropertiesResponse->ulPayloadLength, ( char * ) pxPropertiesResponse->pvMessagePayload ) );

        xAzIoTResult = AzureIoTJSONReader_Init( &xJsonReader, pxPropertiesResponse->pvMessagePayload, pxPropertiesResponse->ulPayloadLength );

        if( xAzIoTResult != eAzureIoTSuccess )
        {
            LogError( ( "AzureIoTJSONReader_Init failed: result 0x%08x", xAzIoTResult ) );
            *pulWritablePropertyResponseBufferLength = 0;
            return e2etestE2E_TEST_FAILED;
        }

        xAzIoTResult = AzureIoTJSONReader_Init( &xJsonReader, pxPropertiesResponse->pvMessagePayload, pxPropertiesResponse->ulPayloadLength );

        if( xAzIoTResult != eAzureIoTSuccess )
        {
            LogError( ( "AzureIoTJSONReader_Init failed: result 0x%08x", xAzIoTResult ) );
            *pulWritablePropertyResponseBufferLength = 0;
            return e2etestE2E_TEST_FAILED;
        }

        /**
         * If the PnP component is for Azure Device Update, function
         * AzureIoTADUClient_SendResponse shall be used to publish back the
         * response for the ADU writable properties.
         * Thus, to prevent this callback to publish a response in duplicate,
         * pulWritablePropertyResponseBufferLength must be set to zero.
         */
        *pulWritablePropertyResponseBufferLength = 0;

        while( ( xAzIoTResult = AzureIoTHubClientProperties_GetNextComponentProperty( pxAzureIoTHubClient, &xJsonReader,
                                                                                      pxPropertiesResponse->xMessageType, eAzureIoTHubClientPropertyWritable,
                                                                                      &pucComponentName, &ulComponentNameLength ) ) == eAzureIoTSuccess )
        {
            LogInfo( ( "Properties component name: %.*s", ulComponentNameLength, pucComponentName ) );

            if( AzureIoTADUClient_IsADUComponent( pxAzureIoTAduClient, pucComponentName, ulComponentNameLength ) )
            {
                /* Mark as received so we can begin with the test process */
                xAduWasReceived = true;
                break;
            }
            else
            {
                LogInfo( ( "Component not ADU: %.*s", ulComponentNameLength, pucComponentName ) );
                prvSkipPropertyAndValue( &xJsonReader );
            }
        }
    }

    return ulStatus;
}


/*-----------------------------------------------------------*/

/**
 * Execute echo command
 *
 * e.g of command issued from service:
 *   {"method":"echo","payload":"hello"}
 *
 **/
static uint32_t prvE2ETestEchoCommandExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                              AzureIoTHubClient_t * pxAzureIoTHubClient,
                                              AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    AzureIoTResult_t xStatus;
    uint16_t usTelemetryPacketID;

    if( ( xStatus = AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                                     xCMD->pulReceivedData,
                                                     xCMD->ulReceivedDataLength,
                                                     NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) ) != eAzureIoTSuccess )
    {
        LogError( ( "Telemetry message send failed!, error code %d", xStatus ) );
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        LogError( ( "Telemetry message PUBACK never received!", xStatus ) );
    }
    else
    {
        LogInfo( ( "Successfully done with prvE2ETestEchoCommandExecute" ) );
    }

    return ( uint32_t ) xStatus;
}
/*-----------------------------------------------------------*/

/**
 * Execute exit command
 *
 * e.g of command issued from service:
 *   {"method":"exit"}
 *
 **/
static uint32_t prvE2ETestExitCommandExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                              AzureIoTHubClient_t * pxAzureIoTHubClient,
                                              AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    ulContinueProcessingCMD = 0;

    return e2etestE2E_TEST_SUCCESS;
}
/*-----------------------------------------------------------*/

/**
 * Execute send initial ADU state command
 *
 * e.g of command issued from service:
 *   {"method":"send_init_adu_state"}
 *
 **/
static uint32_t prvE2ETestSendInitialADUStateExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                                      AzureIoTHubClient_t * pxAzureIoTHubClient,
                                                      AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    uint32_t ulStatus;
    uint16_t usTelemetryPacketID;

    if( AzureIoTADUClient_SendAgentState( pxAzureIoTAduClient, pxAzureIoTHubClient, &xADUDeviceProperties,
                                          NULL, eAzureIoTADUAgentStateIdle, NULL, ucScratchBuffer, sizeof( ucScratchBuffer ), NULL ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Unable to send initial ADU client state!" );
    }

    if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                         xCMD->pulReceivedData,
                                         xCMD->ulReceivedDataLength,
                                         NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
    {
        LogError( ( "Failed to send response" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        ulStatus = e2etestE2E_TEST_FAILED;
        LogError( ( "Telemetry message PUBACK never received!", ulStatus ) );
    }
    else
    {
        ulStatus = e2etestE2E_TEST_SUCCESS;
    }

    prvFreeTwinData( pxTwinMessage );
    pxTwinMessage = NULL;

    return ulStatus;
}
/*-----------------------------------------------------------*/

/**
 * Execute GetTwin for ADU properties command
 *
 * e.g of command issued from service:
 *   {"method":"get_adu_twin"}
 *
 **/
static uint32_t prvE2ETestGetADUTwinPropertiesExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                                       AzureIoTHubClient_t * pxAzureIoTHubClient,
                                                       AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    uint32_t ulStatus;
    uint16_t usTelemetryPacketID;

    if( AzureIoTHubClient_RequestPropertiesAsync( pxAzureIoTHubClient ) != eAzureIoTSuccess )
    {
        LogError( ( "Failed to request twin properties" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( AzureIoTHubClient_ProcessLoop( pxAzureIoTHubClient,
                                            e2etestPROCESS_LOOP_WAIT_TIMEOUT_IN_MSEC ) != eAzureIoTSuccess )
    {
        LogError( ( "receive timeout" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( pxTwinMessage == NULL )
    {
        LogError( ( "Invalid response from server" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                              xCMD->pulReceivedData,
                                              xCMD->ulReceivedDataLength,
                                              NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
    {
        LogError( ( "Failed to send response" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        ulStatus = e2etestE2E_TEST_FAILED;
        LogError( ( "Telemetry message PUBACK never received!", ulStatus ) );
    }
    else
    {
        ulStatus = e2etestE2E_TEST_SUCCESS;
    }

    prvFreeTwinData( pxTwinMessage );
    pxTwinMessage = NULL;

    return ulStatus;
}
/*-----------------------------------------------------------*/

/**
 * Execute application of the update command
 *
 * e.g of command issued from service:
 *   {"method":"apply_update"}
 *
 **/
static uint32_t prvE2ETestApplyADUUpdateExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                                 AzureIoTHubClient_t * pxAzureIoTHubClient,
                                                 AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    uint32_t ulStatus;
    uint16_t usTelemetryPacketID;

    LogInfo( ( "Getting the ADU twin to parse" ) );

    if( AzureIoTHubClient_RequestPropertiesAsync( pxAzureIoTHubClient ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to request twin properties" );
    }

    if( AzureIoTHubClient_ProcessLoop( pxAzureIoTHubClient,
                                       e2etestPROCESS_LOOP_WAIT_TIMEOUT_IN_MSEC ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "receive timeout" );
    }

    if( pxTwinMessage == NULL )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Invalid response from server" );
    }

    LogInfo( ( "Parsing the ADU twin" ) );

    if( prvParseADUTwin( pxAzureIoTHubClient, pxAzureIoTAduClient, pxTwinMessage ) != e2etestE2E_TEST_SUCCESS )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to parse ADU twin!" );
    }

    LogInfo( ( "Authenticating the ADU twin (JWS)" ) );

    if( AzureIoTJWS_ManifestAuthenticate( xAzureIoTAduUpdateRequest.pucUpdateManifest,
                                          xAzureIoTAduUpdateRequest.ulUpdateManifestLength,
                                          xAzureIoTAduUpdateRequest.pucUpdateManifestSignature,
                                          xAzureIoTAduUpdateRequest.ulUpdateManifestSignatureLength,
                                          &xADURootKey[ 0 ],
                                          sizeof( xADURootKey ) / sizeof( xADURootKey[ 0 ] ),
                                          ucAduManifestVerificationBuffer,
                                          sizeof( ucAduManifestVerificationBuffer ) ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to authenticate the JWS manifest!" );
    }

    LogInfo( ( "Sending response to accept the ADU twin" ) );

    if( AzureIoTADUClient_SendResponse(
            pxAzureIoTAduClient,
            pxAzureIoTHubClient,
            eAzureIoTADURequestDecisionAccept,
            ulReceivedTwinVersion,
            ucScratchBuffer,
            sizeof( ucScratchBuffer ),
            NULL ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to send response accepting the properties!" );
    }

    LogInfo( ( "Sending the ADU state as in progress" ) );

    if( AzureIoTADUClient_SendAgentState( pxAzureIoTAduClient, pxAzureIoTHubClient, &xADUDeviceProperties,
                                          NULL, eAzureIoTADUAgentStateDeploymentInProgress, NULL, ucScratchBuffer, sizeof( ucScratchBuffer ), NULL ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Unable to send in progress ADU client state!" );
    }

    LogInfo( ( "Sending completion telemetry" ) );

    if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                         xCMD->pulReceivedData,
                                         xCMD->ulReceivedDataLength,
                                         NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
    {
        LogError( ( "Failed to send response" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        ulStatus = e2etestE2E_TEST_FAILED;
        LogError( ( "Telemetry message PUBACK never received!", ulStatus ) );
    }
    else
    {
        ulStatus = e2etestE2E_TEST_SUCCESS;
    }

    prvFreeTwinData( pxTwinMessage );
    pxTwinMessage = NULL;

    return ulStatus;
}

/*-----------------------------------------------------------*/

/**
 * Execute verification of the ADU update
 *
 * e.g of command issued from service:
 *   {"method":"verify_final_state"}
 *
 **/
static uint32_t prvE2ETestVerifyADUUpdateExecute( E2E_TEST_COMMAND_HANDLE xCMD,
                                                  AzureIoTHubClient_t * pxAzureIoTHubClient,
                                                  AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    uint8_t * ucProvider = "{\"provider\":\"" e2eADU_UPDATE_PROVIDER "\",\"name\":\"";
    uint8_t * ucUpdateName_New = getenv( "e2eADU_UPDATE_NAME" );
    uint8_t * ucVersion = "\",\"version\":\"" e2eADU_UPDATE_VERSION_NEW "\"}";
    uint8_t ucE2eADU_UPDATE_ID_NEW[ strlen( ucProvider ) + strlen( ucUpdateName_New ) + strlen( ucVersion ) ];

    sprintf( ucE2eADU_UPDATE_ID_NEW, "%s%s%s", ucProvider, ucUpdateName_New, ucVersion );

    AzureIoTADUClientDeviceProperties_t xADUDevicePropertiesNew =
    {
        .ucManufacturer                           = ( const uint8_t * ) e2eADU_DEVICE_MANUFACTURER,
        .ulManufacturerLength                     = sizeof( e2eADU_DEVICE_MANUFACTURER ) - 1,
        .ucModel                                  = ( const uint8_t * ) e2eADU_DEVICE_MODEL,
        .ulModelLength                            = sizeof( e2eADU_DEVICE_MODEL ) - 1,
        .ucCurrentUpdateId                        = ( const uint8_t * ) ucE2eADU_UPDATE_ID_NEW,
        .ulCurrentUpdateIdLength                  = sizeof( ucE2eADU_UPDATE_ID_NEW ),
        .ucDeliveryOptimizationAgentVersion       = NULL,
        .ulDeliveryOptimizationAgentVersionLength = 0
    };

    uint32_t ulStatus;
    uint16_t usTelemetryPacketID;

    if( AzureIoTADUClient_SendAgentState( pxAzureIoTAduClient, pxAzureIoTHubClient, &xADUDevicePropertiesNew,
                                          NULL, eAzureIoTADUAgentStateIdle, NULL, ucScratchBuffer, sizeof( ucScratchBuffer ), NULL ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Unable to send initial ADU client state!" );
    }

    if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                         xCMD->pulReceivedData,
                                         xCMD->ulReceivedDataLength,
                                         NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
    {
        LogError( ( "Failed to send response" ) );
        ulStatus = e2etestE2E_TEST_FAILED;
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        ulStatus = e2etestE2E_TEST_FAILED;
        LogError( ( "Telemetry message PUBACK never received!", ulStatus ) );
    }
    else
    {
        ulStatus = e2etestE2E_TEST_SUCCESS;
    }

    prvFreeTwinData( pxTwinMessage );
    pxTwinMessage = NULL;

    return ulStatus;
}

/*-----------------------------------------------------------*/

/**
 * Initialize device command
 *
 **/
static uint32_t prvE2EDeviceCommandInit( E2E_TEST_COMMAND_HANDLE xCMD,
                                         const char * ucData,
                                         uint32_t ulDataLength,
                                         EXECUTE_FN xExecute )
{
    xCMD->xExecute = xExecute;
    xCMD->pulReceivedData = ( const uint8_t * ) ucData;
    xCMD->ulReceivedDataLength = ulDataLength;

    return e2etestE2E_TEST_SUCCESS;
}
/*-----------------------------------------------------------*/

/**
 * DeInitialize device command
 *
 **/
static void prvE2EDeviceCommandDeinit( E2E_TEST_COMMAND_HANDLE xCMD )
{
    vPortFree( ( void * ) xCMD->pulReceivedData );
    memset( xCMD, 0, sizeof( E2E_TEST_COMMAND ) );
}
/*-----------------------------------------------------------*/

/**
 * Find the command and initialized command handle
 *
 **/
static uint32_t prvE2ETestInitializeCMD( const char * pcMethodName,
                                         const char * ucData,
                                         uint32_t ulDataLength,
                                         E2E_TEST_COMMAND_HANDLE xCMD )
{
    uint32_t ulStatus;

    if( !strncmp( e2etestE2E_TEST_ECHO_COMMAND,
                  pcMethodName, sizeof( e2etestE2E_TEST_ECHO_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestEchoCommandExecute );
    }
    else if( !strncmp( e2etestE2E_TEST_EXIT_COMMAND,
                       pcMethodName, sizeof( e2etestE2E_TEST_EXIT_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestExitCommandExecute );
    }
    else if( !strncmp( e2etestE2E_TEST_SEND_INITIAL_ADU_STATE_COMMAND,
                       pcMethodName, sizeof( e2etestE2E_TEST_SEND_INITIAL_ADU_STATE_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestSendInitialADUStateExecute );
    }
    else if( !strncmp( e2etestE2E_TEST_GET_ADU_TWIN_PROPERTIES_COMMAND,
                       pcMethodName, sizeof( e2etestE2E_TEST_GET_ADU_TWIN_PROPERTIES_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestGetADUTwinPropertiesExecute );
    }
    else if( !strncmp( e2etestE2E_TEST_APPLY_ADU_UPDATE_COMMAND,
                       pcMethodName, sizeof( e2etestE2E_TEST_APPLY_ADU_UPDATE_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestApplyADUUpdateExecute );
    }
    else if( !strncmp( e2etestE2E_TEST_VERIFY_ADU_FINAL_STATE_COMMAND,
                       pcMethodName, sizeof( e2etestE2E_TEST_VERIFY_ADU_FINAL_STATE_COMMAND ) - 1 ) )
    {
        ulStatus = prvE2EDeviceCommandInit( xCMD, ucData, ulDataLength,
                                            prvE2ETestVerifyADUUpdateExecute );
    }
    else
    {
        ulStatus = e2etestE2E_TEST_FAILED;
    }

    RETURN_IF_FAILED( ulStatus, "Failed to find a command" );

    return ulStatus;
}
/*-----------------------------------------------------------*/

/**
 * Parse test command received from service
 *
 * */
static uint32_t prvE2ETestParseCommand( uint8_t * pucData,
                                        uint32_t ulDataLength,
                                        E2E_TEST_COMMAND_HANDLE xCMD )
{
    uint32_t ulStatus;
    az_span xJsonSpan;
    az_json_reader xState;
    az_span xMethodName = az_span_create( ucMethodNameBuffer, sizeof( ucMethodNameBuffer ) );

    xJsonSpan = az_span_create( pucData, ulDataLength );

    LogInfo( ( "Command %.*s\r\n", ulDataLength, pucData ) );

    if( az_json_reader_init( &xState, xJsonSpan, NULL ) != AZ_OK )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to parse json" );
    }

    ulStatus = prvGetJsonValueForKey( &xState, e2etestMETHOD_KEY, &xMethodName );
    RETURN_IF_FAILED( ulStatus, "Failed to parse the command" );

    ulStatus = prvE2ETestInitializeCMD( az_span_ptr( xMethodName ), pucData, ulDataLength, xCMD );
    RETURN_IF_FAILED( ulStatus, "Failed to initialize the command" );

    return( ulStatus );
}
/*-----------------------------------------------------------*/

/**
 * Cloud message callback
 *
 * */
void vHandleCloudMessage( AzureIoTHubClientCloudToDeviceMessageRequest_t * pxMessage,
                          void * pvContext )
{
    if( ucC2DCommandData == NULL )
    {
        ucC2DCommandData = pvPortMalloc( pxMessage->ulPayloadLength );
        ulC2DCommandDataLength = pxMessage->ulPayloadLength;

        if( ucC2DCommandData != NULL )
        {
            memcpy( ucC2DCommandData, pxMessage->pvMessagePayload, pxMessage->ulPayloadLength );
        }
        else
        {
            printf( "Failed to allocated memory for cloud message message" );
            configASSERT( false );
        }
    }
}
/*-----------------------------------------------------------*/

/**
 * Device twin message callback
 *
 * */
void vHandlePropertiesMessage( AzureIoTHubClientPropertiesResponse_t * pxMessage,
                               void * pvContext )
{
    if( pxTwinMessage == NULL )
    {
        pxTwinMessage = pvPortMalloc( sizeof( AzureIoTHubClientPropertiesResponse_t ) );

        if( pxTwinMessage == NULL )
        {
            printf( "Failed to allocated memory for Twin message" );
            configASSERT( false );
            return;
        }

        memcpy( pxTwinMessage, pxMessage, sizeof( AzureIoTHubClientPropertiesResponse_t ) );

        if( pxMessage->ulPayloadLength != 0 )
        {
            pxTwinMessage->pvMessagePayload = pvPortMalloc( pxMessage->ulPayloadLength );

            if( pxTwinMessage->pvMessagePayload != NULL )
            {
                memcpy( ( void * ) pxTwinMessage->pvMessagePayload,
                        pxMessage->pvMessagePayload, pxMessage->ulPayloadLength );
            }
        }
    }
}
/*-----------------------------------------------------------*/

/**
 * Poll for commands and execute commands
 *
 * */
uint32_t ulE2EDeviceProcessCommands( AzureIoTHubClient_t * pxAzureIoTHubClient,
                                     AzureIoTADUClient_t * pxAzureIoTAduClient )
{
    AzureIoTResult_t xResult;
    E2E_TEST_COMMAND xCMD = { 0 };
    uint32_t ulStatus = e2etestE2E_TEST_SUCCESS;
    uint8_t * pucData = NULL;
    AzureIoTHubClientCommandRequest_t * pucMethodData = NULL;
    uint32_t ulDataLength;
    uint8_t * ucErrorReport = NULL;
    uint16_t usTelemetryPacketID;

    if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                         e2etestE2E_TEST_CONNECTED_MESSAGE,
                                         sizeof( e2etestE2E_TEST_CONNECTED_MESSAGE ) - 1,
                                         NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Report connected failed!" );
    }
    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Telemetry message PUBACK never received!" );
    }

    /* Send initial state for device */
    if( AzureIoTADUClient_SendAgentState( pxAzureIoTAduClient, pxAzureIoTHubClient, &xADUDeviceProperties,
                                          NULL, eAzureIoTADUAgentStateIdle, NULL, ucScratchBuffer, sizeof( ucScratchBuffer ), NULL ) != eAzureIoTSuccess )
    {
        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Unable to send initial ADU client state!" );
    }

    /* We will wait until the device receives the ADU payload since the service is inconsistent */
    /* with actually sending the update payload. */
    do
    {
        xResult = AzureIoTHubClient_ProcessLoop( pxAzureIoTHubClient,
                                                 e2etestPROCESS_LOOP_WAIT_TIMEOUT_IN_MSEC );

        if( xResult )
        {
            LogError( ( "Failed to receive data from IoTHUB, error code : %d", xResult ) );
            break;
        }

        if( ucC2DCommandData != NULL )
        {
            pucData = ucC2DCommandData;
            ulDataLength = ulC2DCommandDataLength;
            ucC2DCommandData = NULL;

            if( ( ulStatus = prvE2ETestParseCommand( pucData, ulDataLength, &xCMD ) ) )
            {
                vPortFree( pucData );
                LogError( ( "Failed to parse the command, error code : %d", ulStatus ) );
            }
            else
            {
                if( ( ulStatus = xCMD.xExecute( &xCMD, pxAzureIoTHubClient, pxAzureIoTAduClient ) ) != e2etestE2E_TEST_SUCCESS )
                {
                    LogError( ( "Failed to execute, error code : %d", ulStatus ) );
                }

                prvE2EDeviceCommandDeinit( &xCMD );
            }
        }
        else if( pxTwinMessage != NULL )
        {
            LogInfo( ( "Twin message received\r\n" ) );

            if( !xAduWasReceived )
            {
                prvCheckTwinForADU( pxAzureIoTHubClient, pxAzureIoTAduClient, pxTwinMessage );

                if( xAduWasReceived )
                {
                    LogInfo( ( "ADU message received\r\n" ) );

                    /* Once we receive a twin message, it will have been the ADU payload. */
                    if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                                         e2etestE2E_TEST_ADU_PAYLOAD_RECEIVED,
                                                         sizeof( e2etestE2E_TEST_ADU_PAYLOAD_RECEIVED ) - 1,
                                                         NULL, eAzureIoTHubMessageQoS1, &usTelemetryPacketID ) != eAzureIoTSuccess )
                    {
                        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Report ADU received failed!" );
                    }
                    else if( ( prvWaitForPuback( pxAzureIoTHubClient, usTelemetryPacketID ) ) )
                    {
                        RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Telemetry message PUBACK never received!" );
                    }
                }
            }

            free( pxTwinMessage );
            pxTwinMessage = NULL;
        }
        else
        {
            if( !xAduWasReceived )
            {
                if( AzureIoTHubClient_SendTelemetry( pxAzureIoTHubClient,
                                                     e2etestE2E_TEST_WAITING_FOR_ADU,
                                                     sizeof( e2etestE2E_TEST_WAITING_FOR_ADU ) - 1,
                                                     NULL, eAzureIoTHubMessageQoS0, NULL ) != eAzureIoTSuccess )
                {
                    RETURN_IF_FAILED( e2etestE2E_TEST_FAILED, "Failed to send ADU waiting telem!" );
                }
            }

            vTaskDelay( 10 * 1000 / portTICK_PERIOD_MS );
        }
    } while( ( ulStatus == e2etestE2E_TEST_SUCCESS ) &&
             ulContinueProcessingCMD );

    return ulStatus;
}
/*-----------------------------------------------------------*/

/**
 * Get time from fix start point
 *
 * */
uint64_t ulGetUnixTime( void )
{
    return time( NULL );
}
/*-----------------------------------------------------------*/

/**
 * Connect to endpoint with backoff
 *
 * */
TlsTransportStatus_t xConnectToServerWithBackoffRetries( const char * pHostName,
                                                         uint32_t port,
                                                         NetworkCredentials_t * pxNetworkCredentials,
                                                         void * pxNetworkContext )
{
    TlsTransportStatus_t xNetworkStatus;
    BackoffAlgorithmStatus_t xBackoffAlgStatus = BackoffAlgorithmSuccess;
    BackoffAlgorithmContext_t xReconnectParams;
    uint16_t usNextRetryBackOff = 0U;

    /* Initialize reconnect attempts and interval. */
    BackoffAlgorithm_InitializeParams( &xReconnectParams,
                                       e2etestRETRY_BACKOFF_BASE_MS,
                                       e2etestRETRY_MAX_BACKOFF_DELAY_MS,
                                       e2etestRETRY_MAX_ATTEMPTS );

    do
    {
        LogInfo( ( "Creating a TLS connection to %s:%u.\r\n", pHostName, port ) );
        xNetworkStatus = TLS_FreeRTOS_Connect( pxNetworkContext,
                                               pHostName, port,
                                               pxNetworkCredentials,
                                               e2etestTRANSPORT_SEND_RECV_TIMEOUT_MS,
                                               e2etestTRANSPORT_SEND_RECV_TIMEOUT_MS );

        if( xNetworkStatus != TLS_TRANSPORT_SUCCESS )
        {
            xBackoffAlgStatus = BackoffAlgorithm_GetNextBackoff( &xReconnectParams, uxRand(), &usNextRetryBackOff );

            if( xBackoffAlgStatus == BackoffAlgorithmRetriesExhausted )
            {
                LogError( ( "Connection to the broker failed, all attempts exhausted." ) );
            }
            else if( xBackoffAlgStatus == BackoffAlgorithmSuccess )
            {
                LogWarn( ( "Connection to the broker failed. "
                           "Retrying connection with backoff and jitter." ) );
                vTaskDelay( pdMS_TO_TICKS( usNextRetryBackOff ) );
            }
        }
    } while( ( xNetworkStatus != TLS_TRANSPORT_SUCCESS ) && ( xBackoffAlgStatus == BackoffAlgorithmSuccess ) );

    return xNetworkStatus;
}
/*-----------------------------------------------------------*/

/**
 * Calculate HMAC using mbedtls crypto API's
 *
 **/
uint32_t ulCalculateHMAC( const uint8_t * pucKey,
                          uint32_t ulKeyLength,
                          const uint8_t * pucData,
                          uint32_t ulDataLength,
                          uint8_t * pucOutput,
                          uint32_t ulOutputLength,
                          uint32_t * pulBytesCopied )
{
    uint32_t ulResult;

    if( ulOutputLength < e2etestE2E_HMAC_MAX_SIZE )
    {
        return 1;
    }

    mbedtls_md_context_t xContext;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_init( &xContext );

    if( mbedtls_md_setup( &xContext, mbedtls_md_info_from_type( md_type ), 1 ) ||
        mbedtls_md_hmac_starts( &xContext, pucKey, ulKeyLength ) ||
        mbedtls_md_hmac_update( &xContext, pucData, ulDataLength ) ||
        mbedtls_md_hmac_finish( &xContext, pucOutput ) )
    {
        ulResult = 1;
    }
    else
    {
        ulResult = 0;
    }

    mbedtls_md_free( &xContext );
    *pulBytesCopied = e2etestE2E_HMAC_MAX_SIZE;

    return ulResult;
}
/*-----------------------------------------------------------*/
