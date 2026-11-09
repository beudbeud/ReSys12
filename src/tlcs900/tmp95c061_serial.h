// ReSys10: the TMP95C061 serial channel of MAME's tmp95c061_serial.cpp (BSD-3-Clause, MAME contributors), its
// byte-granularity path only (no driver here binds the TXD pin): a transmit completes at once (INTTX), a received byte
// raises INTRX when the receiver is on.
#ifndef RESYS10_TMP95C061_SERIAL_H
#define RESYS10_TMP95C061_SERIAL_H

#include "emu.h"

class tmp95c061_serial_device : public device_t
{
public:
	tmp95c061_serial_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0)
		: device_t(mconfig, "tmp95c061_serial", tag, owner, clock) {}

	// (binders by value, as MAME's: tmp95c061.h hands them out through auto)
	devcb_write8::binder setint() { return m_setint_cb.bind(); }
	devcb_write8::binder tx_byte() { return m_tx_byte_cb.bind(); }
	devcb_write8::binder txd() { return m_txd_cb.bind(); } // (bit-granularity path: not modelled)
	void rxd_w(int) {}
	void rx_byte(uint8_t data)
	{
		if (!(r.mod & MOD_RXE))
			return;
		r.rx_data = data;
		m_setint_cb(0x08);
	}
	void set_pin_enabled(bool en) { r.pin_enabled = en; }

	uint8_t scbuf_r() { return r.rx_data; }
	void scbuf_w(uint8_t data)
	{
		m_setint_cb(0x80);
		if ((r.mod & MOD_SM_MASK) != 0 || r.pin_enabled)
			m_tx_byte_cb(data);
	}
	uint8_t sccr_r() { const uint8_t reg = r.cr; r.cr &= 0xe3; return reg; } // the error flags clear on read
	void sccr_w(uint8_t data) { r.cr = data; }
	uint8_t scmod_r() { return r.mod; }
	void scmod_w(uint8_t data) { r.mod = data; }
	uint8_t brcr_r() { return r.brcr; }
	void brcr_w(uint8_t data) { r.brcr = data; }

	void device_reset() override { r.cr &= 0x80; r.mod &= 0x80; r.brcr = 0; r.rx_data = 0; }

	struct { uint8_t cr, mod, brcr, rx_data; bool pin_enabled; } r{}; // (saved with the CPU)

private:
	static constexpr uint8_t MOD_SM_MASK = 0x0c;
	static constexpr uint8_t MOD_RXE = 0x20;
	devcb_write8 m_setint_cb;
	devcb_write8 m_tx_byte_cb;
	devcb_write8 m_txd_cb;
};

template <typename T> inline void TMP95C061_SERIAL(machine_config &, T, int) {}

#endif
