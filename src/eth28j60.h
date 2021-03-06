
#pragma once

class Eth28J60 {
	public:
		void begin(const uint8_t * mac, uint8_t cs_pin = 10, uint16_t max_frame_len = 1536);
		uint16_t receive(void * packet);
		bool send(const void * packet, uint16_t len);
		void setMacAddr(const uint8_t * mac);

	private:
		uint8_t cs_pin;
		uint8_t cur_bank;
		uint16_t rx_ptr;
		uint16_t max_frame;
		uint16_t tx_start;

		uint8_t regRead(uint8_t reg);
		uint16_t regRead16(uint8_t reg);
		void regWrite(uint8_t reg, uint8_t val);
		void regWrite16(uint8_t reg, uint16_t val);
		void regBitSet(uint8_t reg, uint8_t mask);
		void regBitClear(uint8_t reg, uint8_t mask);
		void phyWrite(uint8_t addr, uint16_t val);

		void bufferWrite(const void * data, uint16_t len);
		void bufferRead(void * data, uint16_t len);

		void bankSet(uint8_t reg);

		void opWrite(uint8_t cmd, uint8_t addr, uint8_t val);
		uint8_t opRead(uint8_t cmd, uint8_t addr);

		void beginTransaction();
		void endTransaction();
};
