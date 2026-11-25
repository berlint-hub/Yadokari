// Run on a host: c++ -std=c++17 switch/app/tests/hotkeys_test.cpp -o /tmp/hotkeys_test && /tmp/hotkeys_test
#include "../HorizonHotkeys.h"
#include <cassert>

using namespace HorizonHotkeys;
int main()
{
	constexpr uint64_t minus = 1, plus = 2, r = 4, l = 8, left = 16, right = 32, up = 64;
	Chords chords({minus, plus, r, l, left, right, up});
	assert(chords.Update(minus, true).action == Action::None);
	auto save = chords.Update(minus | r, true);
	assert(save.action == Action::Save && save.consumed == (minus | r));
	assert(chords.Update(minus | r, true).action == Action::None);
	assert(chords.Update(r, true).consumed == r);
	assert(chords.Update(0, true).consumed == 0);
	assert(chords.Update(minus | l, true).action == Action::Load);
	chords.Update(0, true);
	assert(chords.Update(minus | left, true).action == Action::PreviousSlot);
	chords.Update(0, true);
	assert(chords.Update(minus | right, true).action == Action::NextSlot);
	chords.Update(0, true);
	assert(chords.Update(minus | up, true).action == Action::Screenshot);
	chords.Update(0, true);
	assert(chords.Update(minus | l | r, true).action == Action::None);
	assert(chords.Update(minus | l, true).action == Action::None);
	chords.Update(0, true);
	assert(chords.Update(minus | plus | r, true).action == Action::None);
	assert(chords.Update(minus | r, true).action == Action::None);
	chords.Update(0, true);
	assert(chords.Update(minus | r, false).action == Action::None);
	assert(chords.Update(minus | r, true).action == Action::None);
	chords.Update(0, true);
	assert(chords.Update(minus | r, true).action == Action::Save);
}
