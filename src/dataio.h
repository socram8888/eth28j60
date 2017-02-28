
#pragma once
#include <stdint.h>

class DataReader {
	public:
		virtual uint16_t read(uint8_t * data, uint16_t len);

		bool read(uint8_t * data);
		bool read(uint16_t * data);
		bool read(uint32_t * data);
		bool read(uint64_t * data);
		bool read(int8_t * data);
		bool read(int16_t * data);
		bool read(int32_t * data);
		bool read(int64_t * data);

		virtual void close();
};

class DataWriter {
	public:
		virtual uint16_t write(const uint8_t * data, uint16_t len);

		bool write(uint8_t data);
		bool write(uint16_t data);
		bool write(uint32_t data);
		bool write(uint64_t data);
		bool write(int8_t data);
		bool write(int16_t data);
		bool write(int32_t data);
		bool write(int64_t data);

		virtual void close();
};
