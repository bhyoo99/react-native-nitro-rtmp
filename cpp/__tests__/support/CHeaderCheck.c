/* Compiled as C11: the facade header must be plain C (Swift's clang importer
 * and the JNI file both include it). */
#include "nitrortmp_c.h"

const char* nitrortmp_c_header_check(void) {
  nitrortmp_endpoint_t endpoint;
  nitrortmp_stats_t stats;
  nitrortmp_metadata_t metadata = {0};
  (void)endpoint;
  (void)stats;
  (void)metadata;
  return nitrortmp_state_name(NITRORTMP_STATE_IDLE);
}
