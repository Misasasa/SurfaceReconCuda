
#include <thrust/device_vector.h>
#include <thrust/extrema.h>
#include <thrust/reduce.h>
#include <thrust/sort.h>

#include "SurfaceReconstructorCUDA.h"
#include "MarchingCubeCUDA.h"
#include "kernel_functions.h"


extern __constant__ int device_indexMap[1024];
extern __constant__ SPHHelper device_sphhelper;

void SurfaceReconstructorCUDA::LoadConfig(const char* config) {
	XMLDocument doc;
	int xmlState = doc.LoadFile("config.xml");
	Tinyxml_Reader reader;

	XMLElement* param = doc.FirstChildElement("SurfaceReconstruction")->FirstChildElement(config);
	reader.Use(param);
	particleSpacing = reader.GetFloat("particleSpacing");
	infectRadius = reader.GetFloat("infectRadius");
	float paddingx = reader.GetFloat("padding");
	padding.Set(paddingx, paddingx, paddingx);
	surfaceCellWidth = reader.GetFloat("surfaceCellWidth");

	sphHelper.SetupCubic(infectRadius*0.5);
	normThres = reader.GetFloat("normThreshold");
	neighborThres = reader.GetInt("neighborThreshold");
	isoValue = reader.GetFloat("isoValue");

	cudaMemcpyToSymbol(device_sphhelper, &sphHelper, sizeof(SPHHelper));
}

void SurfaceReconstructorCUDA::ExtractSurface() {
	LoadParticle();
	SetupGrids();

	ExtractSurfaceParticles();
	ExtractSurfaceVertices();

	ComputeScalarValues();
	Triangulate();
	Release();
}

void SurfaceReconstructorCUDA::LoadParticle() {
	cout<<"loading particle...\n";
	particleData.LoadFromFile_CUDA(inputFile.c_str());
	particleData.Analyze();

	surfaceParticleMark = new int[particleData.size()];
	cudaMalloc(&device_surfaceParticleMark, sizeof(int)*particleData.size());
}

void SurfaceReconstructorCUDA::SetupGrids() {
	cout<<"preparing grids...\n";
	
	
	zGrid.BindParticles(particleData);
	cfloat3 min, max;
	min = particleData.xmin - padding*2;
	max = particleData.xmax + padding*2;
	zGrid.SetSize(min, max, infectRadius);
	zGrid.AllocateDeviceBuffer();
	zGrid.BindParticles(particleData);

	
	min = particleData.xmin - padding;
	max = particleData.xmax + padding;
	surfaceGrid.SetSize(min, max, particleSpacing*0.5);
	surfaceGrid.AllocateDeviceBuffer();
}


void SurfaceReconstructorCUDA::ExtractSurfaceParticles() {
	cout<<"extracting surface particles\n";

	SortParticles();
	ComputeColorFieldAndMarkParticles();
}


void SurfaceReconstructorCUDA::SortParticles() {
	ComputeParticleHash_Host(zGrid);

	thrust::sort_by_key(
		thrust::device_ptr<uint>(zGrid.particleHashes),
		thrust::device_ptr<uint>(zGrid.particleHashes + zGrid.numParticles),
		thrust::device_ptr<uint>(zGrid.particleIndices)
	);

	ReorderDataAndFindCellStart_Host(zGrid);
	particleData.CopyFromDevice();
}



void SurfaceReconstructorCUDA::ComputeColorFieldAndMarkParticles() {
	ComputeColorField_Host(zGrid, 
		particleSpacing,
		infectRadius,
		normThres,
		neighborThres,
		device_surfaceParticleMark);
	cudaMemcpy(surfaceParticleMark, device_surfaceParticleMark, sizeof(int)*particleData.size(), cudaMemcpyDeviceToHost);
}

void SurfaceReconstructorCUDA::ExtractSurfaceVertices() {
	cout<<"extracing surface vertices\n";

	cfloat3 aabbLen = cfloat3(infectRadius, infectRadius, infectRadius);
	int numSurfaceVertices = 0;

	for (int i=0; i<particleData.size(); i++){
		if (surfaceParticleMark[i]==0)
			continue;
		cfloat3& x = particleData.GetParticle(i).pos;
		//AABB bounding box
		cfloat3 boxMin = x - aabbLen;
		cfloat3 boxMax = x + aabbLen;
		cint3 coordMin = GetCoordinate(boxMin, surfaceGrid.xmin, surfaceGrid.cellWidth) + cint3(1, 1, 1);
		cint3 coordMax = GetCoordinate(boxMax, surfaceGrid.xmin, surfaceGrid.cellWidth);

		for (int xx=coordMin.x; xx<=coordMax.x; xx++)
			for (int yy=coordMin.y; yy<=coordMax.y; yy++)
				for (int zz=coordMin.z; zz<=coordMax.z; zz++) {
					cint3 coord(xx, yy, zz);
					surfaceGrid.InsertSurfaceVertex(coord);
				}
	}
	surfaceGrid.numSurfaceVertices = surfaceGrid.surfaceVertices->size();
	printf("surface vertices: %d\n", surfaceGrid.numSurfaceVertices);
	surfaceGrid.CopyToDevice();
}


void SurfaceReconstructorCUDA::ComputeScalarValues() {
	cout<<"computing scalar values\n";
	ComputeScalarValues_Host(
		zGrid,
		surfaceGrid,
		particleSpacing,
		infectRadius
	);
	surfaceGrid.CopyToHost();
}

void SurfaceReconstructorCUDA::Triangulate() {
	cout<<"triangulating\n";

	MarchingCubeCUDA marchingCube;
	marchingCube.surfaceGrid = & surfaceGrid;
	marchingCube.SetCubeWidth(surfaceGrid.cellWidth);
	marchingCube.isoLevel = isoValue;

	marchingCube.Marching();
	marchingCube.mesh.Output(outFile.c_str());
}

void SurfaceReconstructorCUDA::Release() {
	delete[] surfaceParticleMark;
	cudaFree(device_surfaceParticleMark);

	particleData.Release();
	zGrid.Release();
	surfaceGrid.Release();
}