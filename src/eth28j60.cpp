
#include <Arduino.h>
#include <SPI.h>

#include "eth28j60.h"
#include "eth28j60_regs.h"

#define BUFFER_LEN 8192

// Maybe this should be configurable?
#define MAXFRAME 1536

/*
 * Errata:
 * The receive hardware maintains an internal Write
 * Pointer which defines the area in the receive buffer
 * where bytes arriving over the Ethernet are written.
 * This internal Write Pointer should be updated with
 * the value stored in ERXST whenever the Receive
 * Buffer Start Pointer, ERXST, or the Receive Buffer
 * End Pointer, ERXND, is written to by the host
 * microcontroller. Sometimes, when ERXST or
 * ERXND is written to, the exact value, 0000h, is
 * stored in the internal receive Write Pointer instead
 * of the ERXST address
 *
 * Workaround:
 * Use the lower segment of the buffer memory for
 * the receive buffer, starting at address 0000h. For
 * example, use the range (0000h to n) for the
 * receive buffer and ((n + 1) to 8191) for the transmit
 * buffer.
 */
#define RXSTART 0
#define RXEND TXSTART
#define TXSTART (BUFFER_LEN - MAXFRAME)

static const SPISettings SPI_SETTINGS(4000000, MSBFIRST, SPI_MODE0);

void Eth28J60::begin(const uint8_t * mac, uint8_t cs_pin) {
	// Initialize class variables
	this->cs_pin = cs_pin;
	this->cur_bank = 0xFF;
	this->rx_ptr = 0;

	// Initialize SPI if not ready
	SPI.begin();

	// Set CS pin as output
	pinMode(cs_pin, OUTPUT);

	// Issue soft reset
	beginTransaction();
	SPI.transfer(CMDSR);
	endTransaction();

	/*
	 * Errata:
	 * After sending an SPI Reset command, the PHY
	 * clock is stopped but the ESTAT.CLKRDY bit is not
	 * cleared. Therefore, polling the CLKRDY bit will not
	 * work to detect if the PHY is read.
	 *
	 * Workaround:
	 * After issuing the Reset command, wait at least
	 * 1 ms in firmware for the device to be read
	 */
	delay(1);

	// Setup Rx buffer
	regWrite16(ERXST, RXSTART);
	regWrite16(ERXND, RXEND);
	regWrite16(ERXRDPT, RXSTART);
	this->rx_ptr = RXSTART;

	// Setup Tx buffer
	regWrite16(ETXST, TXSTART);
	regWrite16(ETXND, TXSTART);
	regWrite16(EWRPT, TXSTART);

	// Setup MAC
	regWrite(MACON1, MACON1_TXPAUS | MACON1_RXPAUS | MACON1_MARXEN); // Enable flow control, Enable MAC Rx
	regWrite(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN | MACON3_FULDPX); // Enable padding, enable CRC & frame len check
	regWrite16(MAMXFL, MAXFRAME);
	regWrite(MABBIPG, 0x15); // Set inter-frame gap
	regWrite(MAIPGL, 0x12);
	regWrite(MAIPGH, 0x00);
	setMacAddr(mac); // Set MAC address

	// Setup PHY
	phyWrite(PHCON1, PHCON1_PDPXMD); // Force full-duplex mode
	phyWrite(PHLCON, PHLCON_LEDA_LINK_STATUS | PHLCON_LEDB_TXRX_ACTIVITY | PHLCON_LFRQ0 | PHLCON_STRCH);

	// Enable Rx packets
	regBitSet(ECON1, ECON1_RXEN);

	return true;
}

bool Eth28J60::send(const void * packet, uint16_t len) {
	if (len > MAXFRAME) {
		return false;
	}

	// Wait until last packet is sent
	while (regRead(ECON1) & ECON1_TXRTS) {
		// TXRTS may not clear - Eth28J60 bug. We must reset transmit logic in case of Tx error
		if (regRead(EIR) & EIR_TXERIF) {
			regBitSet(ECON1, ECON1_TXRST);
			regBitClear(ECON1, ECON1_TXRST);
		}
	}

	// Set pointers (write pointer, start pointer, end pointer)
	regWrite16(EWRPT, TXSTART);
	regWrite16(ETXST, TXSTART);
	regWrite16(ETXND, TXSTART + len);

	// Write per-packet control byte
	opWrite(CMDWBM, 0, 0x00);

	// Write packet
	bufferWrite(packet, len);

	// Send the contents of the transmit buffer onto the network
	regBitSet(ECON1, ECON1_TXRTS);

	return true;
}

size_t Eth28J60::receive(void * packet, size_t max_len) {
	if (regRead(EPKTCNT) == 0) {
		return 0;
	}

	
}

void Eth28J60::setMacAddr(const uint8_t * mac) {
	regWrite(MAADR5, mac[0]);
	regWrite(MAADR4, mac[1]);
	regWrite(MAADR3, mac[2]);
	regWrite(MAADR2, mac[3]);
	regWrite(MAADR1, mac[4]);
	regWrite(MAADR0, mac[5]);
}

uint8_t Eth28J60::regRead(uint8_t reg) {
	bankSet(reg);
	return opRead(CMDRCR, reg);
}

void Eth28J60::regWrite(uint8_t reg, uint8_t val) {
	bankSet(reg);
	opWrite(CMDWCR, reg, val);
}

void Eth28J60::regWrite16(uint8_t reg, uint16_t val) {
	bankSet(reg);
	opWrite(CMDWCR, reg, val);
	opWrite(CMDWCR, reg + 1, val >> 8);
}

void Eth28J60::regBitSet(uint8_t reg, uint8_t mask) {
	bankSet(reg);
	opWrite(CMDBFS, reg, mask);
}

void Eth28J60::regBitClear(uint8_t reg, uint8_t mask) {
	bankSet(reg);
	opWrite(CMDBFC, reg, mask);
}

void Eth28J60::phyWrite(uint8_t reg, uint16_t val) {
	regWrite(MIREGADR, reg);
	regWrite16(MIWR, val);
	delayMicroseconds(11); // 10.24us
	while (regRead(MISTAT) & MISTAT_BUSY); // TODO: timeout
}

void ENC28J60::bufferWrite(const uint8_t * packet, uint16_t len) {
	beginTransaction();

	SPI.transfer(CMDWBM);
	while (len) {
		SPI.transfer(*packet);
		packet++;
		len--;
	}

	endTransaction();
}

void Eth28J60::bankSet(uint8_t reg) {
	uint8_t addr = reg & ADDR_MASK;

	// These are available in all banks
	if (addr >= EIE) {
		return;
	}

	uint8_t bank = (reg & BANK_MASK) >> 5;
	if (bank == cur_bank) {
		return;
	}

	opWrite(CMDBFC, ECON1, ECON1_BSEL1 | ECON1_BSEL0);
	opWrite(CMDBFS, ECON1, bank);
	cur_bank = bank;
}

void Eth28J60::opWrite(uint8_t cmd, uint8_t addr, uint8_t val) {
	uint8_t buf[2];
	buf[0] = cmd | (addr & ADDR_MASK);
	buf[1] = val;

	beginTransaction();
	SPI.transfer(buf, 2);
	endTransaction();
}

uint8_t Eth28J60::opRead(uint8_t cmd, uint8_t addr) {
	uint8_t buf[3];

	uint8_t len = 2;
	// MAC-MI reads return an invalid byte before the actual data
	if (addr & MACMII) {
		len = 3;
	}

	buf[0] = cmd | (addr & ADDR_MASK);
	buf[1] = 0xFF;
	buf[2] = 0xFF;

	beginTransaction();
	SPI.transfer(buf, len);
	endTransaction();

	return buf[len - 1];
}

void Eth28J60::beginTransaction() {
	SPI.beginTransaction(SPI_SETTINGS);

	// Enable device now
	digitalWrite(this->cs_pin, LOW);
}

void Eth28J60::endTransaction() {
	// Disable SPI device
	digitalWrite(this->cs_pin, HIGH);

	SPI.endTransaction();
}
