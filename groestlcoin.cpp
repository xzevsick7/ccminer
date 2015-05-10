
#include <string.h>
#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif
#include <openssl/sha.h>

#include "uint256.h"
#include "sph/sph_groestl.h"
#include "cuda_groestlcoin.h"

#include "miner.h"
#include <cuda.h>
#include <cuda_runtime.h>

#define SWAP32(x) \
    ((((x) << 24) & 0xff000000u) | (((x) << 8) & 0x00ff0000u)   | \
      (((x) >> 8) & 0x0000ff00u) | (((x) >> 24) & 0x000000ffu))

// CPU-groestl
extern "C" void groestlhash(void *state, const void *input)
{
    sph_groestl512_context ctx_groestl;

    //these uint512 in the c++ source of the client are backed by an array of uint32
    uint32_t hashA[16], hashB[16];

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512 (&ctx_groestl, input, 80); //6
    sph_groestl512_close(&ctx_groestl, hashA); //7

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512 (&ctx_groestl, hashA, 64); //6
    sph_groestl512_close(&ctx_groestl, hashB); //7

    memcpy(state, hashB, 32);
}

static bool init[MAX_GPUS] = { false };

extern int scanhash_groestlcoin(int thr_id, uint32_t *pdata, uint32_t *ptarget,
    uint32_t max_nonce, uint32_t *hashes_done)
{
    uint32_t start_nonce = pdata[19]++;
	unsigned int intensity = (device_sm[device_map[thr_id]] > 500) ? 24 : 23;
	uint32_t throughput = device_intensity(device_map[thr_id], __func__, 1U << intensity);
	throughput = min(throughput, max_nonce - start_nonce);

    uint32_t *outputHash = (uint32_t*)malloc(throughput * 16 * sizeof(uint32_t));

    if (opt_benchmark)
        ptarget[7] = 0x000000ff;

    // init
    if(!init[thr_id])
    {
		cudaSetDevice(device_map[thr_id]);
		cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
		if (opt_n_gputhreads == 1)
		{
			cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
		}

		groestlcoin_cpu_init(thr_id, throughput);
        init[thr_id] = true;
    }

    // Endian Drehung ist notwendig
    uint32_t endiandata[32];
    for (int kk=0; kk < 32; kk++)
        be32enc(&endiandata[kk], pdata[kk]);

    // Context mit dem Endian gedrehten Blockheader vorbereiten (Nonce wird sp�ter ersetzt)
    groestlcoin_cpu_setBlock(thr_id, endiandata, (void*)ptarget);

    do {
        // GPU
        uint32_t foundNounce[2];
        const uint32_t Htarg = ptarget[7];

        groestlcoin_cpu_hash(thr_id, throughput, pdata[19], outputHash, foundNounce);

        if(foundNounce[0] < 0xffffffff)
        {
            uint32_t tmpHash[8];
            endiandata[19] = SWAP32(foundNounce[0]);
            groestlhash(tmpHash, endiandata);

            if (tmpHash[7] <= Htarg && fulltest(tmpHash, ptarget))
			{
				int res = 1;
				// check if there was some other ones...
				*hashes_done = pdata[19] - start_nonce + throughput;
				if (foundNounce[1] != 0xffffffff)
				{
					endiandata[19] = SWAP32(foundNounce[1]);
					groestlhash(tmpHash, endiandata);
				if (tmpHash[7] != Htarg) applog(LOG_INFO, "GPU #%d: result for nonce $%08X does not validate on CPU!", thr_id, foundNounce);

					if (tmpHash[7] <= Htarg && fulltest(tmpHash, ptarget))
					{

						pdata[21] = foundNounce[1];
						res++;
						if (opt_benchmark)
							applog(LOG_INFO, "GPU #%d Found second nounce %08x", device_map[thr_id], foundNounce[1]);
					}
					else
					{
						if (tmpHash[7] != Htarg)
						{
							applog(LOG_WARNING, "GPU #%d: result for %08x does not validate on CPU!", device_map[thr_id], foundNounce[1]);
						}
					}
				}
				pdata[19] = foundNounce[0];
				if (opt_benchmark)
					applog(LOG_INFO, "GPU #%d Found nounce %08x", device_map[thr_id], foundNounce[0]);
				return res;
			}
			else
			{
				if (tmpHash[7] != Htarg)
					{
						applog(LOG_WARNING, "GPU #%d: result for %08x does not validate on CPU!", device_map[thr_id], foundNounce[0]);
					}
			}
        }

		pdata[19] += throughput;
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			applog(LOG_ERR, "GPU #%d: %s", device_map[thr_id], cudaGetErrorString(err));
			exit(EXIT_FAILURE);
		}
	} while (!work_restart[thr_id].restart && ((uint64_t)max_nonce > ((uint64_t)(pdata[19]) + (uint64_t)throughput)));

    *hashes_done = pdata[19] - start_nonce + 1;
    free(outputHash);
    return 0;
}

