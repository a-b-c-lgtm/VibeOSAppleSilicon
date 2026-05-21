/* vendor/testcerts/test_chain_ec.c -- chapter 112e.
 *
 * Sibling of test_chain.c.  Re-exports BearSSL 0.6's sample EC
 * (P-256 / ECDSA-SHA-256) cert chain and private key
 * (vendor/bearssl/samples/chain-ec.h, key-ec.h, MIT-licensed by
 * Thomas Pornin) as extern symbols other TUs can link against.
 *
 * The two sample chain headers (RSA and EC) both define CERT0 /
 * CERT1 / CHAIN as `static const`, so each must live in its own
 * translation unit.  Result: linking both test_chain.o AND
 * test_chain_ec.o into the same binary gives us two simultaneous
 * server identities (one RSA, one EC) plus -- crucially for
 * chapter 112e -- two trust anchors to load into the multi-
 * anchor store, so we can prove the store actually carries more
 * than one entry and that the X.509 minimal validator honours
 * both RSA and ECDSA chain signatures.
 *
 * The leaf's CN is "localhost" (same as the RSA chain) and the
 * validity window is the same 2010..2037 range, so the same
 * SYS_GETTIMEOFDAY-fed clock that satisfies chain-rsa satisfies
 * chain-ec too.
 */

#include "bearssl.h"

#include "chain-ec.h"
#include "key-ec.h"

const br_x509_certificate *const test_server_chain_ec     = CHAIN;
const size_t                     test_server_chain_ec_len = CHAIN_LEN;
const br_ec_private_key   *const test_server_key_ec       = &EC;
