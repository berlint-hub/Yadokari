// c++ -std=c++17 switch/app/tests/cheat_validation_test.cpp app/src/main/cpp/pcsx2/ImGui/HorizonCheatValidation.cpp -o /tmp/cheat-validation && /tmp/cheat-validation
#include "../../../app/src/main/cpp/pcsx2/ImGui/HorizonCheatDownload.h"
#include <cassert>

int main()
{
	using namespace HorizonCheatDownload;
	assert(IsSafeSerial("SLUS-20588"));
	assert(!IsSafeSerial("../SLUS-20588"));
	assert(!IsSafeSerial(""));
	assert(!IsSafeSerial("SLUS-20588/other"));
	assert(IsSafeCheatFile("gametitle=Game\n[Unlimited Health]\ndescription=Test\npatch=1,EE,12345678,word,00000001\n"));
	assert(IsSafeCheatFile("// patch=ignored\n[Code]\n patch = 1,EE,123,word,1 // comment\n"));
	assert(!IsSafeCheatFile("patch=1,EE,123,word,1\n[Code]\npatch=1,EE,123,word,1\n"));
	assert(!IsSafeCheatFile("[Code] // invalid header after comment removal\npatch=1,EE,123,word,1\n"));
	assert(!IsSafeCheatFile("dpatch=1,EE,123\n[Code]\npatch=1,EE,123,word,1\n"));
	assert(!IsSafeCheatFile("[Code]\npatch=1,EE,123,word,1\n[]\npatch=1,EE,123,word,1\n"));
	assert(!IsSafeCheatFile("<html>Not found</html>"));
	assert(!IsSafeCheatFile("[Code]\ndescription=No code here\n"));
	assert(!IsSafeCheatFile(std::string("[Code]\npatch=1\0", 15)));
}
