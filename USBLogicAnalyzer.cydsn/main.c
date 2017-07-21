/* ========================================
 *
 * Copyright Minatsu Tukisima, 2017
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * ========================================
 */
#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include "command.h"

/* USBFS Configuration. */
#define USBFS__DEVICE               (0u)
#define USBFS_INTERFACE             (0u)
#define IN_EP_NUM                   (2u)
#define MAX_PACKET_SIZE             (64u)
#define CH                          (2u)
uint8 epBuf[MAX_PACKET_SIZE];

/* Defines for DMA */
#define CAPTURE_SIZE                (MAX_PACKET_SIZE/CH*20u)
#define NUM_OF_BUFFERS              (20u)
#define BUFFER_SIZE                 (CAPTURE_SIZE * NUM_OF_BUFFERS)
#define DMA_BYTES_PER_BURST         1
#define DMA_REQUEST_PER_BURST       1
#define DMA_SRC_BASE                (CYDEV_PERIPH_BASE)
#define DMA_DST_BASE_1              (CY_PSOC5LP) ? ((uint32) dmaBuf_1) : (CYDEV_SRAM_BASE)
#define DMA_DST_BASE_2              (CY_PSOC5LP) ? ((uint32) dmaBuf_2) : (CYDEV_SRAM_BASE)

/* Variable declarations for DMA. */
uint8 dmaBuf_1[BUFFER_SIZE];
uint8 dmaBuf_2[BUFFER_SIZE];
uint8 DMA_1_Chan;
uint8 DMA_2_Chan;
uint8 DMA_1_TD[NUM_OF_BUFFERS];
uint8 DMA_2_TD[NUM_OF_BUFFERS];
volatile uint16 readyBufIdx = 0u;

/* Define DMA base clock. */
#define CLK__HZ                     (64000000U)

/* fx2lafw variables. */
uint8 vendor_cmd = 0u;
struct cmd_start_acquisition cmd_param;

/* For debug print. */
char dbuf[256];
#define DP(...)                     {sprintf(dbuf, __VA_ARGS__); DP_PutString(dbuf);}

/* Prototypes. */
void allocDMAs(void);
void initDMAs(void);
CY_ISR_PROTO(ISR_DmaDone);

int main(void) {
    uint8 isRunning = 0u;
    uint16 transferIdx = 0u;
    uint32 totalTransferCount = 0u;

    /* Enable global interrupts. */
    CyGlobalIntEnable;

    /* Start Debug Port. */
    DP_Start();
    DP_PutString("\n\nPSoC5 USB Logic Analyzer\n");

    /* Stop Clock_1. */
    Clock_1_Stop();

    /* Start BitClkFrequency capture event ISR. */
    DmaDone_StartEx(&ISR_DmaDone);

    /* Allocate DMAs and TDs. */
    allocDMAs();

    /* Start USBFS. */
    USBFS_Start(USBFS__DEVICE, USBFS_5V_OPERATION);

    /* Wait for device enumeration. */
    while (0u == USBFS_GetConfiguration()) ;

    for (;;) {
        /* Check if configuration or interface settings are changed. */
        if (0u != USBFS_IsConfigurationChanged()) {
            /* Check active alternate setting. */
            if ((0u != USBFS_GetConfiguration()) &&
                (0u == USBFS_GetInterfaceSetting(USBFS_INTERFACE))) {
                /* Alternate settings 1: Audio is streaming. */
                DP("Interface Active.\n");
            } else {
                /* Alternate settings 0: Audio is not streaming (mute). */
                DP("Interface Disabled.\n");
            }
        }

        /* Send captured data to the host. */
        if (isRunning) {
            /* If capturing to a buffer is done, send it to the host. */
            uint16 capturedBuf = readyBufIdx;
            uint16 dist = (transferIdx - capturedBuf + BUFFER_SIZE) % BUFFER_SIZE;
            if (dist>0 && dist<=CAPTURE_SIZE) {
                DP("transferidx=%d\n",transferIdx);
                DP("capturedBuf=%d\n",capturedBuf);
                DP("Captured %ld smpls.\n",totalTransferCount);
                Clock_1_Stop();
                CyDmaChDisable(DMA_1_Chan);
                CyDmaChDisable(DMA_2_Chan);
                isRunning = 0u;
            }

            /* Check that IN endpoint buffer is empty before write data. */
            if (USBFS_IN_BUFFER_EMPTY == USBFS_GetEPState(IN_EP_NUM)) {
                if (((capturedBuf - transferIdx + BUFFER_SIZE) % BUFFER_SIZE)>=MAX_PACKET_SIZE/CH) {
                    for (uint8 i = 0u; i < MAX_PACKET_SIZE/CH; i++) {
                        epBuf[i*CH+0] = dmaBuf_1[(transferIdx + i)%BUFFER_SIZE];
                        epBuf[i*CH+1] = dmaBuf_2[(transferIdx + i)%BUFFER_SIZE];
                    }
                    transferIdx = (transferIdx + MAX_PACKET_SIZE/CH) % BUFFER_SIZE;
                    totalTransferCount += MAX_PACKET_SIZE/CH;
                    USBFS_LoadInEP(IN_EP_NUM, epBuf, MAX_PACKET_SIZE);
                }
            }
        }

        /* If start command is arrived from the host, start capturing. */
        if (vendor_cmd != 0u) {
            DP("CMD=%d (0x%02x)\n", vendor_cmd, vendor_cmd);
            DP("flag=%02x DelayH=0x%02x(%d) DelayL=0x%02x(%d)\n", cmd_param.flags,
                cmd_param.sample_delay_h, cmd_param.sample_delay_h,
                cmd_param.sample_delay_l, cmd_param.sample_delay_l);
            
            /* Calculate clock divider value. */
            uint32 baseClk = (cmd_param.flags & CMD_START_FLAGS_CLK_48MHZ) ? 48000000 : 30000000;
            uint32 targetFs = baseClk / (cmd_param.sample_delay_h*0x100 + cmd_param.sample_delay_l + 1u);
            Clock_1_SetDividerValue(CLK__HZ/targetFs);
            DP("Target sampling rate=%ld\n",targetFs);

            /* Initialize variables for capturing. */
            isRunning = 1u;
            readyBufIdx = 0u;
            transferIdx = 0u;
            totalTransferCount = 0u;

            /* Initialize and start DMAs. */
            initDMAs();
            Clock_1_Start();
            
            vendor_cmd = 0u;
        }
    }
}

/*******************************************************************************
*  Handling USB vendor requests.
*******************************************************************************/
#define USBFS_HANDLE_VENDOR_RQST_CALLBACK
static uint8 FW_VERSION[2] = {1, 2};
static uint8 REVID_VERSION[1] = {4};
uint8 USBFS_HandleVendorRqst_Callback() {
    uint8 requestHandled = USBFS_FALSE;

    for (int i = 0; i < 8; i++) {
        DP("ep0Data[%d]=%d (0x%02x)\n", i, USBFS_EP0_DR_BASE.epData[i], USBFS_EP0_DR_BASE.epData[i]);
    }

    /* Check request direction: D2H or H2D. */
    if (0u != (USBFS_bmRequestTypeReg & USBFS_RQST_DIR_D2H)) {
        /* Handle direction from device to host. */
        switch (USBFS_bRequestReg) {
        case CMD_GET_FW_VERSION:
            USBFS_currentTD.pData = FW_VERSION;
            USBFS_currentTD.count = 2;
            requestHandled = USBFS_InitControlRead();
            break;

        case CMD_GET_REVID_VERSION:
            USBFS_currentTD.pData = REVID_VERSION;
            USBFS_currentTD.count = 1;
            requestHandled = USBFS_InitControlRead();
            break;

        default:
            break;
        }
    } else {
        /* Handle direction from host to device. */
        switch (USBFS_bRequestReg) {
        case CMD_START:
            vendor_cmd = USBFS_bRequestReg;
            USBFS_currentTD.pData = (uint8 *)&cmd_param;
            USBFS_currentTD.count = sizeof(struct cmd_start_acquisition);
            requestHandled = USBFS_InitControlWrite();
            break;

        default:
            break;
        }
    }

    return requestHandled;
}

/* Allocate DMA channels and TDs. */
void allocDMAs() {
    /* DMA Configuration for DMA_1. */
    DMA_1_Chan = DMA_1_DmaInitialize(DMA_BYTES_PER_BURST, DMA_REQUEST_PER_BURST,
                                     HI16(DMA_SRC_BASE), HI16(DMA_DST_BASE_1));
    DMA_2_Chan = DMA_2_DmaInitialize(DMA_BYTES_PER_BURST, DMA_REQUEST_PER_BURST,
                                     HI16(DMA_SRC_BASE), HI16(DMA_DST_BASE_2));

    /* Allocate TDs. */
    for (uint8 i = 0; i < NUM_OF_BUFFERS; i++) {
        DMA_1_TD[i] = CyDmaTdAllocate();
        DMA_2_TD[i] = CyDmaTdAllocate();
    }
}

/* Initialize and start DMA. */
void initDMAs() {
    /* Configure and chain TDs. */
    for (uint16 i = 0; i < NUM_OF_BUFFERS; i++) {
        CyDmaTdSetConfiguration(DMA_1_TD[i], CAPTURE_SIZE, DMA_1_TD[(i+1)%NUM_OF_BUFFERS],
                                DMA_1__TD_TERMOUT_EN | CY_DMA_TD_INC_DST_ADR);
        CyDmaTdSetConfiguration(DMA_2_TD[i], CAPTURE_SIZE, DMA_2_TD[(i+1)%NUM_OF_BUFFERS],
                                CY_DMA_TD_INC_DST_ADR);
        CyDmaTdSetAddress(DMA_1_TD[i], LO16((uint32)Pin_1__PS), LO16((uint32) &dmaBuf_1[i*CAPTURE_SIZE]));
        CyDmaTdSetAddress(DMA_2_TD[i], LO16((uint32)Pin_2__PS), LO16((uint32) &dmaBuf_2[i*CAPTURE_SIZE]));
    }

    /* Start DMA Operation. */
    CyDmaChSetInitialTd(DMA_1_Chan, DMA_1_TD[0]);
    CyDmaChSetInitialTd(DMA_2_Chan, DMA_2_TD[0]);
    CyDmaChEnable(DMA_1_Chan, 1u);
    CyDmaChEnable(DMA_2_Chan, 1u);
}

/*******************************************************************************
*  The Interrupt Service Routine for a DMA transfer completion event. The DMA is
*  stopped when there is no data to send.
*******************************************************************************/
CY_ISR(ISR_DmaDone) {
    /* Increment captured position in the buffer. */
    readyBufIdx = (readyBufIdx + CAPTURE_SIZE) % BUFFER_SIZE;
}

/* [] END OF FILE */
