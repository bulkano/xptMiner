#include"global.h"
#include "OpenCLObjects.h"
#include "ticker.h"
#include "metiscoinMiner.h"

#define STEP_SIZE 0x80000
#define NUM_STEPS 0x100
#define STEP_MULTIPLIER 0x10000

MetiscoinOpenCL::MetiscoinOpenCL(int _device_num) {
	this->device_num = _device_num;
	printf("Initializing GPU %d\n", device_num);
	OpenCLMain &main = OpenCLMain::getInstance();
	OpenCLDevice* device = main.getDevice(device_num);
	printf("Initializing Device: %s\n", device->getName().c_str());

//	std::vector<std::string> files_metis;
//	files_metis.push_back("opencl/metis.cl");
//	OpenCLProgram* program_metis = main.getDevice(device_num)->getContext()->loadProgramFromFiles(files_metis);
//	OpenCLKernel* kernel_metis = program_metis->getKernel("metis512");
//
//	std::vector<std::string> files_shavite;
//	files_shavite.push_back("opencl/shavite.cl");
//	OpenCLProgram* program_shavite = main.getDevice(device_num)->getContext()->loadProgramFromFiles(files_shavite);
//	OpenCLKernel* kernel_shavite = program_shavite->getKernel("shavite512");

	std::vector<std::string> files_keccak;
	files_keccak.push_back("opencl/keccak.cl");
	files_keccak.push_back("opencl/shavite.cl");
	files_keccak.push_back("opencl/metis.cl");
	files_keccak.push_back("opencl/miner.cl");
	OpenCLProgram* program = device->getContext()->loadProgramFromFiles(files_keccak);
#ifdef SINGLE_KERNEL
	kernel_all = program->getKernel("metiscoin_process_noinit");
#else
	kernel_keccak_noinit = program->getKernel("keccak_step_noinit");
	kernel_shavite = program->getKernel("shavite_step");
	kernel_metis = program->getKernel("metis_step");
#endif
	main.listDevices();

	u = device->getContext()->createBuffer(25*sizeof(cl_ulong), CL_MEM_READ_WRITE, NULL);
	buff = device->getContext()->createBuffer(4, CL_MEM_READ_WRITE, NULL);

#ifndef SINGLE_KERNEL
	hashes = device->getContext()->createBuffer(
			64 * STEP_SIZE, CL_MEM_READ_WRITE, NULL);
#endif
	out = device->getContext()->createBuffer(sizeof(cl_uint) * 255, CL_MEM_READ_WRITE, NULL);
	out_count = device->getContext()->createBuffer(sizeof(cl_uint), CL_MEM_READ_WRITE, NULL);
	q = device->getContext()->createCommandQueue(device);
}

static inline cl_uint my_swap32(cl_uint x)
{
  return ((((x)&    0xFF)<<24) | (((x)&    0xFF00)<<8) | \
	  (((x)&0xFF0000)>> 8) | (((x)&0xFF000000)>>24));
}

void MetiscoinOpenCL::metiscoin_process(minerMetiscoinBlock_t* block)
{

	block->nonce = 0;
	uint32 target = *(uint32*)(block->targetShare+28);
	OpenCLDevice* device = OpenCLMain::getInstance().getDevice(device_num);
	//printf("processing block with Device: %s\n", device->getName().c_str());


	// measure time
	for (uint32 n = 0; n < NUM_STEPS; n++)
	{
#ifdef MEASURE_TIME
		uint32 begin = getTimeMilliseconds();
#endif
		if( block->height != monitorCurrentBlockHeight )
			break;
#ifndef SINGLE_KERNEL
		//keccak
		//kernel void keccak_step_noinit(constant const ulong* u, constant const char* buff, global ulong* out, uint begin_nonce)
		kernel_keccak_noinit->resetArgs();
		kernel_keccak_noinit->addGlobalArg(u);
		kernel_keccak_noinit->addGlobalArg(buff);
		kernel_keccak_noinit->addGlobalArg(hashes);
		kernel_keccak_noinit->addScalarUInt(n * STEP_SIZE);

#endif
		sph_keccak512_context	 ctx_keccak;
		sph_keccak512_init(&ctx_keccak);
		sph_keccak512(&ctx_keccak, &block->version, 80);

		q->enqueueWriteBuffer(u, ctx_keccak.u.wide, 25*sizeof(cl_ulong));
		q->enqueueWriteBuffer(buff, ctx_keccak.buf, 4);
#ifndef SINGLE_KERNEL
		q->enqueueKernel1D(kernel_keccak_noinit, STEP_SIZE,
				kernel_keccak_noinit->getWorkGroupSize(device));

#ifdef MEASURE_TIME
		printf("keccak work group size = %d\n", kernel_keccak_noinit->getWorkGroupSize(device));
		q->finish();
		uint32 end_keccak = getTimeMilliseconds();
#endif

		// shavite
		kernel_shavite->resetArgs();
		kernel_shavite->addGlobalArg(hashes);

		q->enqueueKernel1D(kernel_shavite, STEP_SIZE,
				kernel_shavite->getWorkGroupSize(device));

#ifdef MEASURE_TIME
		printf("shavite work group size = %d\n", kernel_shavite->getWorkGroupSize(device));
		q->finish();
		uint32 end_shavite = getTimeMilliseconds();
#endif
		// metis
		// metis_step(global ulong* in, global uint* out, global uint* outcount, uint begin_nonce, uint target) {
		kernel_metis->resetArgs();
		kernel_metis->addGlobalArg(hashes);
		kernel_metis->addGlobalArg(out);
		kernel_metis->addGlobalArg(out_count);
		kernel_metis->addScalarUInt(n*STEP_SIZE);
		kernel_metis->addScalarUInt(target);

#endif
		cl_uint out_count_tmp = 0;
		q->enqueueWriteBuffer(out_count, &out_count_tmp, sizeof(cl_uint));
#ifdef SINGLE_KERNEL
		kernel_all->resetArgs();
		kernel_all->addGlobalArg(u);
		kernel_all->addGlobalArg(buff);
		kernel_all->addGlobalArg(out);
		kernel_all->addGlobalArg(out_count);
		kernel_all->addScalarUInt(n*STEP_SIZE);
		kernel_all->addScalarUInt(target);

		q->enqueueWriteBuffer(u, ctx_keccak.u.wide, 25*sizeof(cl_ulong));
		q->enqueueWriteBuffer(buff, ctx_keccak.buf, 4);
		q->enqueueKernel1D(kernel_all, STEP_SIZE,
				kernel_all->getWorkGroupSize(device));
#else
		q->enqueueKernel1D(kernel_metis, STEP_SIZE,
				kernel_metis->getWorkGroupSize(device));
#endif
		q->enqueueReadBuffer(out, out_tmp, sizeof(cl_uint) * 255);
		q->enqueueReadBuffer(out_count, &out_count_tmp, sizeof(cl_uint));
		q->finish();

		for (int i = 0; i < out_count_tmp; i++) {
			totalShareCount++;
			block->nonce = out_tmp[i];
			xptMiner_submitShare(block);
		}

		totalCollisionCount += STEP_SIZE;
#ifdef MEASURE_TIME
		uint32 end = getTimeMilliseconds();
#ifdef SINGLE_KERNEL
		printf("Elapsed time: %d ms\n", (end-begin));
#else
		printf("Elapsed time: %d (k = %d, s = %d, m = %d) ms\n", (end-begin), (end_keccak-begin), (end_shavite-end_keccak), (end-end_shavite));
#endif
#endif

#ifdef VALIDATE_ALGORITHMS
		uint32 begin_validation = getTimeMilliseconds();

#ifndef SINGLE_KERNEL
		cl_ulong *tmp_hashes = new cl_ulong[8 * STEP_SIZE];
		q->enqueueReadBuffer(hashes, tmp_hashes,
				sizeof(cl_ulong) * 8 * STEP_SIZE);
		q->finish();

		int aaa = 0;
		for (int i = 0; i < STEP_SIZE; i++) {
			cl_ulong * hhh = tmp_hashes+(i*8);
			if( *(cl_uint*)((cl_uchar*)hhh+28) <= target )
			{
				aaa++;
			}
		}

		if (aaa != out_count_tmp) {
			printf ("************* ERRO ****************\n");
			exit(0);
		}


		// validator
		for (int f2 = 0; f2 < STEP_SIZE/0x8000; f2++) {
		block->nonce = (n*STEP_SIZE/0x8000) * STEP_MULTIPLIER + f2 * STEP_MULTIPLIER;
		for (int f = 0; f < 0x8000; f++) {
			sph_keccak512_context	 ctx_keccak;
			sph_shavite512_context	 ctx_shavite;
			sph_metis512_context	 ctx_metis;
			cl_ulong hash0[8];
			cl_ulong hash1[8];
			cl_ulong hash2[8];
			cl_ulong *hash1_2;
			cl_ulong *hash2_2;

			sph_keccak512_init(&ctx_keccak);
			sph_shavite512_init(&ctx_shavite);
			sph_metis512_init(&ctx_metis);
			sph_keccak512(&ctx_keccak, &block->version, 80);
			sph_keccak512_close(&ctx_keccak, hash0);
			sph_shavite512(&ctx_shavite, hash0, 64);
			sph_shavite512_close(&ctx_shavite, hash1);
			sph_metis512(&ctx_metis, hash1, 64);
			sph_metis512_close(&ctx_metis, hash2);

			hash2_2 = tmp_hashes+(f*8)+(f2*8*0x8000);

			for (int i = 0; i < 8; i++) {
				if (hash2[i] != hash2_2[i]) {
					printf ("**** Hashes do not match %i %x %x\n", i, hash0[i], hash2_2[i]);
				}
			}

			block->nonce++;
		}
		}
		delete tmp_hashes;
#endif
		block->nonce = 0;
		uint32 end_validation = getTimeMilliseconds();
		printf("Validation time: %d ms\n", (end_validation-begin_validation));
#endif
	}

}
