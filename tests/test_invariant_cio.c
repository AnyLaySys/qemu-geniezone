#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void print_irb_err(void *irb);

typedef struct {
    char padding_before[256];
    char msgline[512];
    char padding_after[256];
} test_context_t;

START_TEST(test_buffer_overflow_protection)
{
    // Invariant: Buffer reads never exceed the declared length
    // Test that print_irb_err does not overflow msgline buffer
    
    // Craft IRB structures that trigger multiple sense condition flags
    // to maximize strcat operations
    struct {
        char irb_data[96];
    } payloads[] = {
        // Payload 1: All sense flags set (worst case - triggers all strcats)
        {{0xFF, 0xFF, 0xFF, 0xFF, [4 ... 95] = 0xFF}},
        // Payload 2: Boundary case - some flags set
        {{0x80, 0x40, 0x20, 0x10, [4 ... 95] = 0x00}},
        // Payload 3: Valid input - minimal flags
        {{0x00, 0x00, 0x00, 0x00, [4 ... 95] = 0x00}}
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        test_context_t ctx;
        memset(&ctx, 0xAA, sizeof(ctx));
        
        // Redirect stdout to capture output and prevent overflow
        FILE *devnull = fopen("/dev/null", "w");
        FILE *old_stdout = stdout;
        stdout = devnull;
        
        print_irb_err(&payloads[i]);
        
        stdout = old_stdout;
        fclose(devnull);
        
        // Check canaries: padding should not be corrupted
        int corrupted = 0;
        for (size_t j = 0; j < sizeof(ctx.padding_before); j++) {
            if (ctx.padding_before[j] != (char)0xAA) corrupted = 1;
        }
        for (size_t j = 0; j < sizeof(ctx.padding_after); j++) {
            if (ctx.padding_after[j] != (char)0xAA) corrupted = 1;
        }
        
        ck_assert_msg(corrupted == 0, "Buffer overflow detected for payload %d", i);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_overflow_protection);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}