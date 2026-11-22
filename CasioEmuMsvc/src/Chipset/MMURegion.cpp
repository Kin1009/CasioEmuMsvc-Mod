#include "MMURegion.hpp"

#include "Emulator.hpp"
#include "Chipset.hpp"
#include "MMU.hpp"
#include <functional>

namespace casioemu
{
	MMURegion::MMURegion()
	{
		setup_done = false;
		emulator = nullptr;
	}

	MMURegion::~MMURegion()
	{
		if (setup_done)
			Kill();
	}

	void MMURegion::Setup(size_t _base, size_t _size, std::string _description, void *_userdata, ReadFunction _read, WriteFunction _write, Emulator &_emulator)
	{
		if (setup_done)
			PANIC("Setup invoked twice\n");

		emulator = &_emulator;
		base = _base;
		size = _size;
		description = _description;
		userdata = _userdata;
		read = _read;
		write = _write;

		emulator->chipset.mmu.RegisterRegion(this);
		setup_done = true;
	}

	void MMURegion::Kill()
	{
		// Board models skip chipset.Setup(), so some MMURegions are never
		// registered and `emulator` stays null. Teardown calls Kill() from
		// Chipset::DestructPeripherals / DestructClockGenerator /
		// DestructInterruptSFR as well as ~MMURegion(). Stay a no-op when the
		// region was never set up, and stay idempotent for double teardown.
		if (!setup_done || !emulator)
		{
			setup_done = false;
			return;
		}
		emulator->chipset.mmu.UnregisterRegion(this);
		setup_done = false;
	}
}

