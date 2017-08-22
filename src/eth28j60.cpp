
#include <Arduino.h>
#include <SPI.h>

#include "eth28j60.h"
#include "eth28j60_regs.h"

static const SPISettings SPI_SETTINGS(4000000, MSBFIRST, SPI_MODE0);

#define DEBUG

void Eth28J60::begin(const uint8_t * mac, uint8_t cs_pin, uint16_t max_frame) {
	// Initialize class variables
	this->cs_pin = cs_pin;
	this->cur_bank = 0xFF;
	this->max_frame = max_frame;

	// Calculate TX offset in buffer
	tx_start = BUFFER_LEN - max_frame - sizeof(struct tx_header) - sizeof(struct tx_status);

	// Make tx_start even to workaround an errata in receiving
	tx_start &= 0xFFFE;

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

	// Setup Rx buffer
	rx_ptr = 0x0000;
	regWrite16(ERXST, 0x0000);
	regWrite16(ERXND, tx_start - 1);
	regWrite16(ERXRDPT, 0x0000);

	// Setup TX pointer
	regWrite16(ETXST, tx_start);

	// Setup MAC
	regWrite(MACON1, MACON1_TXPAUS | MACON1_RXPAUS | MACON1_MARXEN); // Enable flow control, Enable MAC Rx
	regWrite(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN | MACON3_FULDPX); // Enable padding, enable CRC & frame len check
	regWrite16(MAMXFL, max_frame + CRC_SIZE);
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
	if (len > max_frame) {
		return false;
	}

	// Wait until last packet is sent
	while (regRead(ECON1) & ECON1_TXRTS);

	// Build control header
	struct tx_header hdr;
	hdr.control = 0x00;

	// Set pointers (write pointer, start pointer, end pointer)
	regWrite16(EWRPT, tx_start);
	regWrite16(ETXND, tx_start + sizeof(hdr) + len - 1);

	// Write per-packet control byte
	bufferWrite(&hdr, sizeof(hdr));

	// Write packet
	bufferWrite(packet, len);

	// Send the contents of the transmit buffer onto the network
	regBitSet(ECON1, ECON1_TXRTS);

	return true;
}

uint16_t Eth28J60::receive(void * packet) {
	if (regRead(EPKTCNT) == 0) {
		return 0;
	}

	uint16_t packet_len = 0;
	struct rx_header hdr;

	do {
		regWrite16(ERDPT, rx_ptr);

		struct rx_header hdr;
		bufferRead(&hdr, sizeof(hdr));

		rx_ptr = hdr.next_packet_pointer;

#ifdef DEBUG
		Serial.print("Packet count: ");
		Serial.println(regRead(EPKTCNT));
		Serial.print("NPP: 0x");
		Serial.println(hdr.next_packet_pointer, HEX);
		Serial.print("Packet length: ");
		Serial.println(hdr.packet_length);
		Serial.print("Status: 0x");
		Serial.println(hdr.status, HEX);
#endif

		if ((hdr.status & RX_HEADER_STATUS_OK) && hdr.packet_length - CRC_SIZE <= max_frame) {
			packet_len = hdr.packet_length - CRC_SIZE;
			bufferRead(packet, packet_len);
		}

		regBitSet(ECON2, ECON2_PKTDEC);
	} while (packet_len == 0 && regRead(EPKTCNT) > 0);

	/*
	 * Errata:
	 * The receive hardware may corrupt the circular
	 * receive buffer (including the Next Packet Pointer
	 * and receive status vector fields) when an even value
	 * is programmed into the ERXRDPTH:ERXRDPTL
	 * registers. 
	 *
	 * Workaround:
	 * Ensure that only odd addresses are written to the
	 * ERXRDPT registers. Assuming that ERXND con-
	 * tains an odd value, many applications can derive a
	 * suitable value to write to ERXRDPT by subtracting
	 * one from the Next Packet Pointer (a value always
	 * ensured to be even because of hardware padding)
	 * and then compensating for a potential ERXST to
	 * ERXND wrap-around. Assuming that the receive
	 * buffer area does not span the 1FFFh to 0000h mem-
	 * ory boundary, the logic in Example 2 will ensure that
	 * ERXRDPT is programmed with an odd value
	 */
	uint16_t rxRdPt = hdr.next_packet_pointer;
	if (rxRdPt % 2 == 0) {
		if (rxRdPt == 0) {
			rxRdPt = tx_start - 1;
		} else {
			rxRdPt--;
		}
	}
	regWrite16(ERXRDPT, rxRdPt);

	return packet_len;
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

uint16_t Eth28J60::regRead16(uint8_t reg) {
	bankSet(reg);
	return opRead(CMDRCR, reg + 1) << 8 | opRead(CMDRCR, reg);
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

void Eth28J60::bufferWrite(const void * data, uint16_t len) {
	const uint8_t * bytes = (const uint8_t *) data;

	beginTransaction();

	SPI.transfer(CMDWBM);
	while (len) {
		SPI.transfer(*bytes);
		bytes++;
		len--;
	}

	endTransaction();
}

void Eth28J60::bufferRead(void * data, uint16_t len) {
	uint8_t * bytes = (uint8_t *) data;

	beginTransaction();

	SPI.transfer(CMDRBM);
	while (len) {
		*bytes = SPI.transfer(0x00);
		bytes++;
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
