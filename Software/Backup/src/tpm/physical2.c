#include <stdio.h>
#include <string.h>


#include "commandStructs.h"
#include "authHandler.h"
#include "globals.h"
#include "UART.h"
#include "physical.h"
#include "utils.h"

#include "twi.h"
#include "spi.h"
//#include "delay.h"
#include "conf_board.h"
#include "tpm_iic.h"


// number of polling attempts to get first ACK
#define POLL_NUM			100
#define SPI_READ_CMD		0x80
#define SPI_WRITE_CMD		0x00
#define WAIT_STATE_MASK		0x0001
#define EXPECT_MORE_DATA	0x08
#define WAIT_STATE_REQUIRED 0x00
#define DUMMY_WAIT_BYTE		0xFF
#define CMD_SIZE			0x04
#define CMDRDY_MASK			0xC8
#define DATARDY_MASK		0x90
#define CMD_OFFSET			0x04
#define TRANSFER_SUCCESS    0x00

// global var to hold transient TWSR value
volatile uint8_t	twst;

#define DUMMY_BYTES		0xFF
#define RESP_SIZE_LSB	5
#define RESP_SIZE_MSB	4
#define MAX_RETRIES		100
#ifdef TWI_INTERFACE

uint8_t tpm_iic_test_data[] = {
	0x00, 0xC1, 0x00, 0x00,
	0x00, 0x0C, 0x00, 0x00,
	0x00, 0x99, 0x00, 0x01
};

//static const 
// Response
// Tag:  0x00 0xC4
// Size: 0x00 0x00 0x00 0x0a
// Return code: 00 00 00 00 (success)

 uint8_t tpm_iic_expected_test_data_resp[] = {
	0x00, 0xC4,
	0x00, 0x00, 0x00, 0x0A,
	0x00, 0x00, 0x00, 0x00
};


/** Data to be sent */
#define  IIC_TEST_DATA_LENGTH  (sizeof(tpm_iic_test_data)/sizeof(uint8_t))

#else

/* Chip select. */
#define SPI_CHIP_SEL 0

/* Clock polarity. */
#define SPI_CLK_POLARITY 0

/* Clock phase. */
#define SPI_CLK_PHASE 1

/* Delay before SPCK. */
#define SPI_DLYBS 0x40 

/* Delay between consecutive transfers. */
#define SPI_DLYBCT 0x40

/* Number of SPI clock configurations. */
#define NUM_SPCK_CONFIGURATIONS 3

/* Data block number. */
#define MAX_DATA_BLOCK_NUMBER  4

/* Slave data state, begin to return last data block. */
#define SPI_CMD_READ     0x80
#define SPI_CMD_WRITE    0x00

/* SPI clock setting (Hz). */
static uint32_t gs_ul_spi_clock;

/* SPI Status. */
//static struct status_block_t gs_spi_status;


/* SPI clock configuration. */
//const uint32_t gs_ul_clock_configurations[] = {1000000, 10000000, 2000000 };

/**
 * \brief Initialize SPI as master.
 */
static void spi_master_initialize(void)
{
	/* Configure an SPI peripheral. */
	spi_enable_clock(SPI_MASTER_BASE);
	spi_disable(SPI_MASTER_BASE);
	spi_reset(SPI_MASTER_BASE);
	spi_set_lastxfer(SPI_MASTER_BASE);
	spi_set_master_mode(SPI_MASTER_BASE);
	spi_disable_mode_fault_detect(SPI_MASTER_BASE);
	
	spi_set_peripheral_chip_select_value(SPI_MASTER_BASE, SPI_CHIP_SEL);
	spi_set_clock_polarity(SPI_MASTER_BASE, SPI_CHIP_SEL, SPI_CLK_POLARITY);
	spi_set_clock_phase(SPI_MASTER_BASE, SPI_CHIP_SEL, SPI_CLK_PHASE);
	spi_set_bits_per_transfer(SPI_MASTER_BASE, SPI_CHIP_SEL,SPI_CSR_BITS_8_BIT);
	spi_set_baudrate_div(SPI_MASTER_BASE, SPI_CHIP_SEL,(sysclk_get_cpu_hz() / gs_ul_spi_clock));
	spi_set_transfer_delay(SPI_MASTER_BASE, SPI_CHIP_SEL, SPI_DLYBS,SPI_DLYBCT);
	spi_enable(SPI_MASTER_BASE);
}

/**
 * \brief Set the specified SPI clock configuration.
 *
 * \param configuration  Index of the configuration to set.
 */
void spi_set_clock_configuration(uint8_t configuration)
{
	gs_ul_spi_clock = 1000000;//gs_ul_clock_configurations[configuration];
	//printf("Setting SPI clock #%lu ... \n\r", (unsigned long)gs_ul_spi_clock);
	spi_master_initialize();
}

#endif


void twi_init(void)
{
	twi_options_t opt = {
		.master_clk = sysclk_get_cpu_hz(),
		.speed = TWI_CLK,
		.chip  = TPM_DEVICE_ADDRESS
	};

	if (twi_master_setup(TPM_TWI_MODULE, &opt) != TWI_SUCCESS) {
		/* to-do, error handling */
		while(1);
	}

}

//-----------------------------------------------------------------------------
void dumpXferBuf(void)
{
	uint16_t	i;

	for(i=0; i < numBytes; i++)
	{
		if(!(i % 16))
		{
			printf("\r\n");
		}
		printf("%02X ", xferBuf[i] );
	}
	printf("\r\n");
}
//-----------------------------------------------------------------------------
void logStartTries(uint8_t numTries)
{
	printf("%d tries\r\n", numTries);
}
//-----------------------------------------------------------------------------
void convertHexToAsciiString( uint8_t hexData, char *ptrHexString )
{
	ptrHexString[0] = (hexData&0xF0)>>4;
	ptrHexString[0] += (ptrHexString[0] < 0x0A) ? 0x30 : 0x37;

	ptrHexString[1] = (hexData&0x0F);
	ptrHexString[1] += (ptrHexString[1] < 0x0A) ? 0x30 : 0x37;
}


#ifdef SPI_INTERFACE
/**
 * \brief Perform SPI master transfer.
 *
 * \param pbuf Pointer to buffer to transfer.
 * \param size Size of the buffer.
 */
 uint8_t spi_master_transfer(uint8_t *p_buf, uint8_t *resp, uint32_t size)
{
	uint16_t i;
	uint8_t uc_pcs;
	uint16_t data;
	bool updateCnt = false;
	uint8_t retry;
	
	union {
		uint8_t		bytes[2];
		uint16_t	size;
	} paramSize;

	cpu_irq_enter_critical();


	/* send COMMAND bytes */
	for (i = 0; i < CMD_SIZE; i++) {
		spi_write(SPI_MASTER_BASE, *(p_buf+i), 0, 0);
		
		/* Wait transfer done. */
		while ((spi_read_status(SPI_MASTER_BASE) & SPI_SR_RDRF) == 0);
		
		/* Read Data. */
		spi_read(SPI_MASTER_BASE, &data, &uc_pcs);
		//*(p_buf+i) = data;
	}
	
	retry = 0;
	
	/* handle the WAIT STATE before proceeding */
	do
	{
		/* check for wait state */
		if((data & WAIT_STATE_MASK) == WAIT_STATE_REQUIRED)
		{
			spi_write(SPI_MASTER_BASE, 0xFF, 0, 0);
					
			/* Wait transfer done. */
			while ((spi_read_status(SPI_MASTER_BASE) & SPI_SR_RDRF) == 0)
			;
					
			/* Read Data. */
			 spi_read(SPI_MASTER_BASE, &data, &uc_pcs);
		}
		
		if(retry > MAX_RETRIES)
		{
			cpu_irq_leave_critical();
			printf("\n\r Comm Error: WAIT STATE bit was never \
			        \r\n was never  released by TPM device\r\n");
			return 1;
		}
		retry++;
		
	   /* loop 'til completed or retries are exhausted */	
	} while ((data & WAIT_STATE_MASK) == WAIT_STATE_REQUIRED);

	
	/* update receive size */
	if((size == TPM_HEADER_SIZE)&&((*p_buf) == SPI_READ_CMD))
	{
		updateCnt = true;
	}
	
	
	/* process the remaining data */
	if(size > 0)
	{
		/* send payload bytes */
		for (i = 0; i < size; i++) 
		{
			/* transfer data */
			if((*p_buf) == SPI_READ_CMD)
				spi_write(SPI_MASTER_BASE, 0xFF, 0, 0);
			else
				spi_write(SPI_MASTER_BASE, *(p_buf+ CMD_SIZE + i), 0, 0);
		
			/* Wait transfer done. */
			while ((spi_read_status(SPI_MASTER_BASE) & SPI_SR_RDRF) == 0);
		
			/* Read Data. */
			spi_read(SPI_MASTER_BASE, &data, &uc_pcs);
			*(resp+i) = data;
			
			/* READ OPERATION ONLY
			 * extract the paramSize
			 * big/little endian conversion,
			 * this is faster than shifting)
			 */
			if((i == RESP_SIZE_LSB)&&(updateCnt == true))
			{
				paramSize.bytes[0] = *(resp + RESP_SIZE_LSB);
				paramSize.bytes[1] = *(resp + RESP_SIZE_MSB);
				numBytes = paramSize.size;
				updateCnt = false;

				if(numBytes > 1024)
				{
					cpu_irq_leave_critical();
					printf("\r\nretrieving response size\r\n");
					return 1;
				}
				 
				/* update cnt */
				if(paramSize.size != TPM_HEADER_SIZE){
					size = paramSize.size;
				}
			}			
		}		
	}
	
	cpu_irq_leave_critical();

	delay_ms(1);
	return 0;

}
#endif

//-----------------------------------------------------------------------------
void sendCommand(	responseAction 	wantResponse,
					logAction 		wantLog)
{

#ifdef TWI_INTERFACE
	uint16_t 	i;
	uint32_t	retCode;
	twi_packet_t packet_tx, packet_rx;
#else	
	uint8_t cmd_buffer[16];
	uint8_t resp_buffer[20];
	uint8_t retry;
	uint8_t retry_gobit;
	/* this must remain static */
	static bool firstTime = true;
	uint8_t resend_cmd_count;
	
#endif		

	/* show what we're sending */
	if(wantLog == getLog)
	{
		printf("\r\nto TPM:");
		dumpXferBuf();
	}
	


#ifdef TWI_INTERFACE

   /* get the TPM's attention */
	i = 0;

	/* Configure the data packet to be transmitted */
	packet_tx.chip        = TPM_DEVICE_ADDRESS;
	packet_tx.addr[0]     = 0x00;
	packet_tx.addr[1]     = 0x00;
	packet_tx.addr_length = 0x00;
	packet_tx.buffer      = xferBuf;
	packet_tx.length      = numBytes;	

	do{
		retCode =0;
		/* IIC command */
		retCode = twi_master_write(TPM_TWI_MODULE, &packet_tx);							
		i++;
		
		asm("nop");
		/* give POLL_NUM tries... */	 
	} while((retCode != TWI_SUCCESS) && (i < POLL_NUM));

#ifndef NO_STARTBIT_LOG
	if(i > 1)
	{
		printf("\r\nfirst write startBit took ");
		logStartTries(i);
		if(i == POLL_NUM)
		{
			printf("aborting (xmit ACK 1)\r\n");
			return;
		}
	}
#endif


	if(wantResponse == getResponse)
	{

		printf("\r\nwaiting for TPM response...");

		/* clear the xfer buffer */
		for(i=0; i<numBytes; i++)
			xferBuf[i]=0;

		/* wait for TPM to finish
		 * command execution production 
		 * code would want to have 
		 * (multiple-level) timeouts here
		 */
		do{
			retCode = twi_probe(TPM_TWI_MODULE, TPM_DEVICE_ADDRESS);	
		} while(retCode != PASS);


		/* Configure the data packet to be received */
		packet_rx.chip        = packet_tx.chip;
		packet_rx.addr[0]     = 0x00;
		packet_rx.addr[1]     = 0x00;
		packet_rx.addr_length = 0x00;
		packet_rx.buffer      = xferBuf;
		packet_rx.length      = TPM_HEADER_SIZE;

		twi_master_read_tpm(TPM_TWI_MODULE, &packet_rx);


		if( (numBytes > 1023) || (numBytes < 10) )
		{
			/* something's wrong!! */
			printf("bad paramSize(%d)\r\n",numBytes);
			return;	
		}

		/* show what we got */
		if(wantLog == getLog)
		{
			printf("\r\nfrom TPM:");
			dumpXferBuf();
		}
	}

#else // SPI

if(firstTime)
{
	/* check locality register access */	
	cmd_buffer[0] = SPI_READ_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x00;
	spi_master_transfer(&cmd_buffer[0],&resp_buffer[0], 0x01);

 	/* send request use byte */
	cmd_buffer[0] = SPI_WRITE_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x00;
	cmd_buffer[4] = 0x02;
	spi_master_transfer(cmd_buffer,&resp_buffer[0],  0x01);

/**********************************************************
 ************** READ, EXPECT 0xA1 *************************
 *********************************************************/ 
	cmd_buffer[0] = SPI_READ_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x00;
	spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01);

firstTime = false;
}



/***************************************************************
	Write 0x40 to address FED40018 (Status Reg-commandReady)
 ***************************************************************/ 
	cmd_buffer[0] = SPI_WRITE_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x18;
	cmd_buffer[4] = 0x40;
	if(spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01) != TRANSFER_SUCCESS)
	{
		printf("\r\n COMM ERROR: During transfer to FED40018 (0x40.1)... \r\n");
		asm("nop");
	}

/*******************************************************************
	Read address FED40018-> Should return C8 (commandReady/stsValid)
 *******************************************************************/ 

/**********************************************************
 Read address FED40018-> Should return C8 (commandReady/stsValid)
 *********************************************************/ 
	retry_gobit = 0;
	retry = 0;
	while(1)
	{
		
		cmd_buffer[0] = SPI_READ_CMD;
		cmd_buffer[1] = 0xD4;
		cmd_buffer[2] = 0x00;
		cmd_buffer[3] = 0x18;
		spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01);

		/* expected results received */
		if((resp_buffer[0]&CMDRDY_MASK) == 0xC8)
			break;

		if(retry > MAX_RETRIES)
		{	
			printf("\n\r Comm Error: MASK 0xC8 (commandReady/stsValid) \
					\r\n             was never received\r\n");
			return;			
//			goto RETRY_CMDREADY;
		}
		retry++;
		delay_ms(100);
		
	}// while ((resp_buffer[0]&CMDRDY_MASK) != 0xC8);



/**********************************************************
        Next steps are for command communication
Write the following command sequence to the buffer address 
(FED40024)-> 00 C1 00 00 00 0C 00 00 00 99 00 01 
(TPM_Startup command)		
 *********************************************************/ 
resend_cmd_count = 0;


/**********************************************************
	Poll address FED40018 after the command is sent to until
	the value 0x90(dataAvailable) is read.	
*********************************************************/ 	
	retry		= 0;
	while(1) 
	{

		cmd_buffer[0] = SPI_READ_CMD;
		cmd_buffer[1] = 0xD4;
		cmd_buffer[2] = 0x00;
		cmd_buffer[3] = 0x18;
		spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01);
			
		/* expected results received */
		if((resp_buffer[0]&DATARDY_MASK) == DATARDY_MASK)
			break;
		
		/* error, */
		if(retry > MAX_RETRIES)
		{	
			printf("\n\r Comm Error: MASK 0x90 (dataAvailable for reading) \
					\r\n             was never received\r\n");					
			return;
		}

		retry++;			
		delay_ms(100);
	} 


/**********************************************************
Read FIFO (address FED40024) for return data from TPM.
 *********************************************************/ 
	cmd_buffer[0] = SPI_READ_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x24;
	spi_master_transfer(&cmd_buffer[0],&xferBuf[0],  0x0A);
	asm("nop");
	
	if( (numBytes > 1023) || (numBytes < 10) )
	{
		/* somethings wrong!! */
		printf("bad paramSize(%d)\r\n",numBytes);
		return;
	}

	/* show what we got */
	if(wantLog == getLog)
	{
		printf("\r\nfrom TPM:");
		dumpXferBuf();
	}
	
	
/**********************************************************
 Write 0x40 to address FED40018 (Status Reg-commandReady)
 *********************************************************/ 
	cmd_buffer[0] = SPI_WRITE_CMD;
	cmd_buffer[1] = 0xD4;
	cmd_buffer[2] = 0x00;
	cmd_buffer[3] = 0x18;
	cmd_buffer[4] = 0x40;
	
	
	if(spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01) != TRANSFER_SUCCESS)
	{
		printf("\r\n COMM ERROR: During transfer to FED40018 (0x40.2)... \r\n");
		asm("nop");
	}	
	
// 	/* send command 2x */
// 	cmd_buffer[0] = SPI_WRITE_CMD;
// 	cmd_buffer[1] = 0xD4;
// 	cmd_buffer[2] = 0x00;
// 	cmd_buffer[3] = 0x18;
// 	cmd_buffer[4] = 0x40;
// 	spi_master_transfer(&cmd_buffer[0],&resp_buffer[0],  0x01);

	
#endif
}
//-----------------------------------------------------------------------------



