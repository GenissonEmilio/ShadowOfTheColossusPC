// Existing kernel/interrupt suites, run without a ROM or a graphics window.
#include "MiniTest.h"
#include <cstdlib>

void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ee_save_state_tests();
void reset_ps2_test_function_table();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ee_save_state_tests();
    const int result = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(result);
}
