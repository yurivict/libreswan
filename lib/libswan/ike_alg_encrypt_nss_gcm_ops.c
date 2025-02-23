/* NSS GCM for libreswan
 *
 * Copyright (C) 2014,2016,2018 Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * Special advise from Bob Relyea - needs to go before any nss include
 *
 */
#define NSS_PKCS11_2_0_COMPAT 1

#include "lswlog.h"
#include "lswnss.h"
#include "prmem.h"
#include "prerror.h"

#include "constants.h"
#include "ike_alg.h"
#include "ike_alg_encrypt_ops.h"

static bool ike_alg_nss_gcm(const struct encrypt_desc *alg,
			    uint8_t *salt, size_t salt_size,
			    uint8_t *wire_iv, size_t wire_iv_size,
			    uint8_t *aad, size_t aad_size,
			    uint8_t *text_and_tag,
			    size_t text_size, size_t tag_size,
			    PK11SymKey *sym_key, bool enc,
			    struct logger *logger)
{
	/* See pk11gcmtest.c */
	bool ok = true;

	chunk_t salt_chunk = {
		.ptr = salt,
		.len = salt_size,
	};
	chunk_t wire_iv_chunk = {
		.ptr = wire_iv,
		.len = wire_iv_size,
	};
	chunk_t iv = clone_hunk_hunk(salt_chunk, wire_iv_chunk, "IV");

	CK_GCM_PARAMS gcm_params;
	gcm_params.pIv = iv.ptr;
	gcm_params.ulIvLen = iv.len;
	gcm_params.pAAD = aad;
	gcm_params.ulAADLen = aad_size;
	gcm_params.ulTagBits = tag_size * 8;

	SECItem param;
	param.type = siBuffer;
	param.data = (void*)&gcm_params;
	param.len = sizeof gcm_params;

	/* Output buffer for transformed data.  */
	size_t text_and_tag_size = text_size + tag_size;
	uint8_t *out_buf = PR_Malloc(text_and_tag_size);
	unsigned int out_len = 0;

	if (enc) {
		SECStatus rv = PK11_Encrypt(sym_key, alg->nss.mechanism,
					    &param, out_buf, &out_len,
					    text_and_tag_size,
					    text_and_tag, text_size);
		if (rv != SECSuccess) {
			llog_nss_error(RC_LOG, logger,
				       "AEAD encryption using %s_%u and PK11_Encrypt() failed",
				       alg->common.fqn, PK11_GetKeyLength(sym_key) * BITS_PER_BYTE);
			ok = false;
		} else if (out_len != text_and_tag_size) {
			/* should this be a pexpect fail? */
			llog_nss_error(RC_LOG_SERIOUS, logger,
				       "AEAD encryption using %s_%u and PK11_Encrypt() failed (output length of %u not the expected %zd)",
				       alg->common.fqn, PK11_GetKeyLength(sym_key) * BITS_PER_BYTE,
				       out_len, text_and_tag_size);
			ok = false;
		}
	} else {
		SECStatus rv = PK11_Decrypt(sym_key, alg->nss.mechanism, &param,
					    out_buf, &out_len, text_and_tag_size,
					    text_and_tag, text_and_tag_size);
		if (rv != SECSuccess) {
			llog_nss_error(RC_LOG, logger,
				       "AEAD decryption using %s_%u and PK11_Decrypt() failed",
				       alg->common.fqn, PK11_GetKeyLength(sym_key) * BITS_PER_BYTE);
			ok = false;
		} else if (out_len != text_size) {
			/* should this be a pexpect fail? */
			llog_nss_error(RC_LOG_SERIOUS, logger,
				       "AEAD decryption using %s_%u and PK11_Decrypt() failed (output length of %u not the expected %zd)",
				       alg->common.fqn, PK11_GetKeyLength(sym_key) * BITS_PER_BYTE,
				       out_len, text_size);
			ok = false;
		}
	}

	memcpy(text_and_tag, out_buf, out_len);
	PR_Free(out_buf);
	free_chunk_content(&iv);

	return ok;
}

static void nss_gcm_check(const struct encrypt_desc *encrypt, struct logger *logger)
{
	const struct ike_alg *alg = &encrypt->common;
	pexpect_ike_alg(logger, alg, encrypt->nss.mechanism > 0);
}

const struct encrypt_ops ike_alg_encrypt_nss_gcm_ops = {
	.backend = "NSS(GCM)",
	.check = nss_gcm_check,
	.do_aead = ike_alg_nss_gcm,
};
