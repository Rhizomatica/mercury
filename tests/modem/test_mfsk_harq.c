/* Does LLR accumulation across copies decode where a single copy cannot? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mfsk_ldpc.h"

static unsigned s_=12345;
static double urand(void){ s_=s_*1103515245u+12345u; return ((s_>>16)&0x7fff)/32768.0; }
static double gauss(void){ double u=urand()+1e-12,v=urand(); return sqrt(-2*log(u))*cos(2*M_PI*v); }

int main(void)
{
    const mfsk_ldpc_code_t *c = &mfsk_ldpc_8_16;
    if (!c) { printf("no code\n"); return 1; }
    int K=c->K, N=c->N;
    int *info=malloc(sizeof(int)*K), *coded=malloc(sizeof(int)*N);
    int *out=malloc(sizeof(int)*K);
    float *llr=malloc(sizeof(float)*N), *acc=malloc(sizeof(float)*N);

    int single_ok=0, comb_ok=0, trials=200;
    double sigma=1.35;                     /* deep enough that 1 copy usually fails */
    for (int t=0;t<trials;t++){
        for(int i=0;i<K;i++) info[i]=urand()<0.5?0:1;
        mfsk_ldpc_encode(c, info, coded);
        memset(acc,0,sizeof(float)*N);
        int got_single=0;
        for (int copy=0; copy<3; copy++){
            for(int i=0;i<N;i++){
                double x = coded[i]?-1.0:1.0;             /* LLR>0 favours bit 0 */
                llr[i] = (float)((x + sigma*gauss())*2.0/(sigma*sigma));
                if (llr[i]>5) llr[i]=5; if (llr[i]<-5) llr[i]=-5;
                acc[i]+=llr[i];
            }
            if (copy==0 && mfsk_ldpc_decode(c, llr, out, 50)
                && !memcmp(out,info,sizeof(int)*K)) got_single=1;
        }
        if (got_single) single_ok++;
        float avg[MFSK_LDPC_MAXN];
        for(int i=0;i<N;i++) avg[i]=acc[i]/3.0f;
        if (mfsk_ldpc_decode(c, avg, out, 50) && !memcmp(out,info,sizeof(int)*K)) comb_ok++;
    }
    /* Second question: what happens when copies of DIFFERENT codewords are
     * summed?  A listening station cannot know which CALL a copy belongs to --
     * it accumulates before decoding and has no session yet -- so this is what
     * two callers overlapping actually looks like.  The requirement is not that
     * it decodes (it cannot); it is that it never yields a WRONG frame, because
     * the modem gates output on CRC16 and decodes single-shot first. */
    int mixed_false = 0, mixed_decoded = 0;
    for (int t = 0; t < trials; t++) {
        int infoA[MFSK_LDPC_MAXK], infoB[MFSK_LDPC_MAXK];
        for (int i = 0; i < K; i++) infoA[i] = urand() < 0.5 ? 0 : 1;
        for (int i = 0; i < K; i++) infoB[i] = urand() < 0.5 ? 0 : 1;
        mfsk_ldpc_encode(c, infoA, coded);
        memset(acc, 0, sizeof(float) * N);
        for (int i = 0; i < N; i++) {
            double x = coded[i] ? -1.0 : 1.0;
            float v = (float)((x + sigma * gauss()) * 2.0 / (sigma * sigma));
            if (v > 5) v = 5; if (v < -5) v = -5;
            acc[i] += v;
        }
        mfsk_ldpc_encode(c, infoB, coded);            /* a DIFFERENT codeword */
        for (int i = 0; i < N; i++) {
            double x = coded[i] ? -1.0 : 1.0;
            float v = (float)((x + sigma * gauss()) * 2.0 / (sigma * sigma));
            if (v > 5) v = 5; if (v < -5) v = -5;
            acc[i] += v;
        }
        float avg2[MFSK_LDPC_MAXN];
        for (int i = 0; i < N; i++) avg2[i] = acc[i] / 2.0f;
        if (mfsk_ldpc_decode(c, avg2, out, 50)) {
            mixed_decoded++;
            if (!memcmp(out, infoA, sizeof(int) * K) ||
                !memcmp(out, infoB, sizeof(int) * K)) mixed_false++;
        }
    }

    printf("single copy : %3d/%d decoded\n", single_ok, trials);
    printf("3 combined  : %3d/%d decoded\n", comb_ok, trials);
    /* The whole chunked-CALL scheme rests on this: copies that individually
     * fail must decode once their LLRs are combined.  Measured 0/200 -> 200/200
     * at sigma=1.35, so a wide margin is safe and this stays a real assertion
     * rather than a smoke test. */
    if (comb_ok <= single_ok + trials / 2) {
        printf("FAIL: combining did not rescue copies that fail singly\n");
        return 1;
    }
    printf("mixed pair  : %3d/%d converged, %d matched a real codeword\n",
           mixed_decoded, trials, mixed_false);
    /* Summing two different codewords must not reconstruct either one. If this
     * ever fires, the CRC16 gate in modem_mfsk.c is the only thing standing
     * between a stale accumulator and a bogus CALL being acted on. */
    if (mixed_false != 0) {
        printf("FAIL: combining different codewords produced a real frame\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
