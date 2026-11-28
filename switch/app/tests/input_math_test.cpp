// c++ -std=c++17 switch/app/tests/input_math_test.cpp -o /tmp/input-math-test && /tmp/input-math-test
#include "../HorizonInputMath.h"
#include <cassert>
#include <cmath>

int main()
{
	using HorizonInputMath::ApplyDeadzone;
	assert(ApplyDeadzone(0.1f, 0.15f) == 0.0f);
	assert(ApplyDeadzone(-0.15f, 0.15f) == 0.0f);
	assert(std::fabs(ApplyDeadzone(0.575f, 0.15f) - 0.5f) < 0.0001f);
	assert(std::fabs(ApplyDeadzone(-0.575f, 0.15f) + 0.5f) < 0.0001f);
	assert(ApplyDeadzone(1.0f, 0.5f) == 1.0f);
	assert(ApplyDeadzone(-2.0f, 0.5f) == -1.0f);
	assert(ApplyDeadzone(0.25f, 0.0f) == 0.25f);
	assert(ApplyDeadzone(0.5f, 2.0f) == 0.0f);
}
