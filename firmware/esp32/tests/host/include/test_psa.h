#pragma once

#include <stddef.h>

void test_psa_reset(void);
void test_psa_set_wrap_result(int result);
unsigned int test_psa_generate_count(void);
unsigned int test_psa_destroy_count(void);
size_t test_psa_active_key_count(void);
