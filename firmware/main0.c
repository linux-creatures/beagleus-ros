/*
 * HC-SR04 remoteproc/rpmsg project based on TI's lab05 example.
 *
 * Copyright (C) 2015 Dimitar Dimitrov <dinuxbg@gmail.com>
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:

 *Firmware for Left and Right HC-SR04 modules
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pru_cfg.h>
#include <pru_ctrl.h>
#include <pru_iep.h>
#include <pru_intc.h>
#include <rsc_types.h>
#include <pru_virtqueue.h>
#include <pru_rpmsg.h>
#include <sys_mailbox.h>
#include "resource_table_0.h"


/* Host-1 Interrupt sets bit 31 in register R31 */
#define HOST_INT					0x80000000

/* The PRU-ICSS system events used for RPMsg are defined in the Linux device tree
 * PRU0 uses system event 16 (To ARM) and 17 (From ARM)
 * PRU1 uses system event 18 (To ARM) and 19 (From ARM)
 */
#define TO_ARM_HOST			16
#define FROM_ARM_HOST			17

#define CHAN_NAME					"rpmsg-pru"

#define CHAN_DESC					"Channel 30"
#define CHAN_PORT					30

/*
 * Used to make sure the Linux drivers are ready for RPMsg communication
 * Found at linux-x.y.z/include/uapi/linux/virtio_config.h
 */
#define VIRTIO_CONFIG_S_DRIVER_OK	4

char payload[RPMSG_BUF_SIZE/64];
volatile register uint32_t __R30;
volatile register uint32_t __R31;

#define PRU_OCP_RATE_HZ		(200 * 1000 * 1000)

#define TRIG_PULSE_US		10


#define TRIG3_BIT               3
#define ECHO3_BIT               5
#define TRIG4_BIT               2
#define ECHO4_BIT               0




/* A utility function to reverse a string  */
void reverse(char str[], int length)
{
    char temp;
    int start = 0;
    int end = length -1;
    while (start < end)
    {
	temp = str[start];
	str[start] = str[end];
	str[end] = temp;
        start++;
        end--;
    }
}
 
// Implementation of itoa()
char* itoa(int num, char* str, int base)
{
    int i = 0;
    bool isNegative = false;
  
    /* Handle 0 explicitely, otherwise empty string is printed for 0 */
    if (num == 0)
    {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }
 
    // In standard itoa(), negative numbers are handled only with 
    // base 10. Otherwise numbers are considered unsigned.
    if (num < 0 && base == 10)
    {
        isNegative = true;
        num = -num;
    }
 
    // Process individual digits
    while (num != 0)
    {
        int rem = num % base;
        str[i++] = (rem > 9)? (rem-10) + 'a' : rem + '0';
        num = num/base;
    }
 
    // If number is negative, append '-'
    if (isNegative)
        str[i++] = '-';
 
    str[i] = '\0'; // Append string terminator
 
    // Reverse the string
    reverse(str, i);
 
    return str;
}

void hc_sr04_init(void)
{
	__R30 = 0x0000;
}

int hc_sr04_measure_pulse(int TRIG_BIT, int ECHO_BIT)
{
	bool echo, timeout;
	
	/* pulse the trigger for 10us */
	__R30 = 1u << TRIG_BIT;
	__delay_cycles(TRIG_PULSE_US * (PRU_OCP_RATE_HZ / 1000000));
	__R30 = 0u << TRIG_BIT;

	/* Enable counter */
	PRU0_CTRL.CYCLE = 0;
	PRU0_CTRL.CTRL_bit.CTR_EN = 1;

	/* wait for ECHO to get high */
	do {
		echo = !!(__R31 & (1u << ECHO_BIT));
		timeout = PRU0_CTRL.CYCLE > PRU_OCP_RATE_HZ;
	} while (!echo && !timeout);

	PRU0_CTRL.CTRL_bit.CTR_EN = 0;

	if (timeout)
		return -1;

	/* Restart the counter */
	PRU0_CTRL.CYCLE = 0;
	PRU0_CTRL.CTRL_bit.CTR_EN = 1;

	/* measure the "high" pulse length */
	do {
		echo = !!(__R31 & (1u << ECHO_BIT));
		timeout = PRU0_CTRL.CYCLE > PRU_OCP_RATE_HZ;
	} while (echo && !timeout);

	PRU0_CTRL.CTRL_bit.CTR_EN = 0;

	if (timeout)
		return -1;

	uint64_t cycles = PRU0_CTRL.CYCLE;

	return cycles / ((uint64_t)PRU_OCP_RATE_HZ / 1000000);
}


static int measure_distance_mm()
{
	int d_mm3, d_mm4;
	int t_us = hc_sr04_measure_pulse(int TRIG3_BIT, int ECHO3_BIT);
	itoa(d_mm2, payload, 10);	

	/*
	 * Print the distance received from the sonar
	 * At 20 degrees in dry air the speed of sound is 3422 mm/sec
	 * so it takes 2.912 us to make 1 mm, i.e. 5.844 us for a
	 * roundtrip of 1 mm.
	 */
	d_mm = (t_us * 1000) / 5844;
	if (t_us < 0)
		d_mm = -1;

	return d_mm;
}


/*
 * main.c
 */
void main(void)
{
	struct pru_rpmsg_transport transport;
	uint16_t src, dst, len;
	volatile uint8_t *status;

	/* allow OCP master port access by the PRU so the PRU can read external memories */
	CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

	/* clear the status of the PRU-ICSS system event that the ARM will use to 'kick' us */
	CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;

	/* Make sure the Linux drivers are ready for RPMsg communication */
	status = &resourceTable.rpmsg_vdev.status;
	while (!(*status & VIRTIO_CONFIG_S_DRIVER_OK));

	/* Initialize the RPMsg transport structure */
	pru_rpmsg_init(&transport, &resourceTable.rpmsg_vring0, &resourceTable.rpmsg_vring1, TO_ARM_HOST, FROM_ARM_HOST);

	/* Create the RPMsg channel between the PRU and ARM user space using the transport structure. */
	while (pru_rpmsg_channel(RPMSG_NS_CREATE, &transport, CHAN_NAME, CHAN_DESC, CHAN_PORT) != PRU_RPMSG_SUCCESS);

	hc_sr04_init();
	while (1) {
		/* Check bit 31 of register R31 to see if the ARM has kicked us */
		if (__R31 & HOST_INT) {
			/* Clear the event status */
			CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;
			/* Receive all available messages, multiple messages can be sent per kick */
			if (pru_rpmsg_receive(&transport, &src, &dst, payload, &len) == PRU_RPMSG_SUCCESS) {
				/* Echo the message back to the same address from which we just received */
				while(1){
				int d_mm1 = measure_distance_mm(TRIG3_BIT, ECHO3_BIT);
				
				/* there is no room in IRAM for iprintf */
                itoa(d_mm1, payload, 10);

				pru_rpmsg_send(&transport, dst, src, payload, len);
				memset(payload,'\n',strlen(payload));

				pru_rpmsg_send(&transport, dst, src, payload, len);
				/*int d_mm2 = measure_distance_mm(TRIG4_BIT, ECHO4_BIT);
				
                itoa(d_mm2, payload, 10);

				pru_rpmsg_send(&transport, dst, src, payload, len);
				memset(payload,'\n',strlen(payload));
				pru_rpmsg_send(&transport, dst, src, payload, len);*/
			}
			}
		}
	}
}

