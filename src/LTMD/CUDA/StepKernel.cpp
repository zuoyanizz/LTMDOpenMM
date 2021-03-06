#include "LTMD/CUDA/StepKernel.h"

#include <cmath>
#include "SimTKOpenMMUtilities.h"
#include "OpenMM.h"
#include "CudaIntegrationUtilities.h"
#include "CudaKernels.h"
#include "CudaArray.h"
#include "CudaContext.h"
#include "openmm/internal/ContextImpl.h"
#include "LTMD/CUDA/KernelSources.h"
#include "LTMD/Integrator.h"
#include <stdlib.h>
#include <iostream>
using namespace std;

using namespace OpenMM;

extern void kGenerateRandoms( CudaContext *gpu );
void kNMLUpdate( CUmodule *module, CudaContext *gpu, float deltaT, float tau, float kT, int numModes, int &iterations, CudaArray &modes, CudaArray &modeWeights, CudaArray &noiseValues );
void kNMLRejectMinimizationStep( CUmodule *module, CudaContext *gpu, CudaArray &oldpos );
void kNMLAcceptMinimizationStep( CUmodule *module, CudaContext *gpu, CudaArray &oldpos );
void kNMLLinearMinimize( CUmodule *module, CudaContext *gpu, int numModes, float maxEigenvalue, CudaArray &oldpos, CudaArray &modes, CudaArray &modeWeights );
void kNMLQuadraticMinimize( CUmodule *module, CudaContext *gpu, float maxEigenvalue, float currentPE, float lastPE, CudaArray &oldpos, CudaArray &slopeBuffer, CudaArray &lambdaval );
void kFastNoise( CUmodule *module, CudaContext *cu, int numModes, float kT, int &iterations, CudaArray &modes, CudaArray &modeWeights, float maxEigenvalue, CudaArray &noiseVal, CudaArray &oldpos, float stepSize );

double drand() { /* uniform distribution, (0..1] */
	return ( rand() + 1.0 ) / ( RAND_MAX + 1.0 );
}
double random_normal() { /* normal distribution, centered on 0, std dev 1 */
	return sqrt( -2 * log( drand() ) ) * cos( 2 * M_PI * drand() );
}


namespace OpenMM {
	namespace LTMD {
		namespace CUDA {
			StepKernel::StepKernel( std::string name, const Platform &platform, CudaPlatform::PlatformData &data ) : LTMD::StepKernel( name, platform ),
				data( data ), modes( NULL ), modeWeights( NULL ), MinimizeLambda( 0 ) {

				//MinimizeLambda = new CUDAStream<float>( 1, 1, "MinimizeLambda" );
				//MinimizeLambda = new CudaArray( *(data.contexts[0]), 1, sizeof(float), "MinimizeLambda" );
				iterations = 0;
				kIterations = 0;
			}

			StepKernel::~StepKernel() {
				if( modes != NULL ) {
					delete modes;
				}
				if( modeWeights != NULL ) {
					delete modeWeights;
				}
			}

			void StepKernel::initialize( const System &system, const Integrator &integrator ) {
				// TMC This is done automatically when you setup a context now.
				//OpenMM::cudaOpenMMInitializeIntegration( system, data, integrator ); // TMC not sure how to replace
				data.contexts[0]->initialize();
				minmodule = data.contexts[0]->createModule( KernelSources::minimizationSteps );
				linmodule = data.contexts[0]->createModule( KernelSources::linearMinimizers );
				quadmodule = data.contexts[0]->createModule( KernelSources::quadraticMinimizers );
#ifdef FAST_NOISE
				fastmodule = data.contexts[0]->createModule( KernelSources::fastnoises, "-DFAST_NOISE=1" );
				updatemodule = data.contexts[0]->createModule( KernelSources::NMLupdates, "-DFAST_NOISE=1" );
#else
				updatemodule = data.contexts[0]->createModule( KernelSources::NMLupdates, "-DFAST_NOISE=0" );
#endif

				MinimizeLambda = new CudaArray( *( data.contexts[0] ), 1, sizeof( float ), "MinimizeLambda" );
				//data.contexts[0]->getPlatformData().initializeContexts(system);
				mParticles = data.contexts[0]->getNumAtoms();
				//NoiseValues = new CUDAStream<float4>( 1, mParticles, "NoiseValues" );
				NoiseValues = new CudaArray( *( data.contexts[0] ), mParticles, sizeof( float4 ), "NoiseValues" );
				/*for( size_t i = 0; i < mParticles; i++ ){
					(*NoiseValues)[i] = make_float4( 0.0f, 0.0f, 0.0f, 0.0f );
				}*/
				std::vector<float4> tmp( mParticles );
				for( size_t i = 0; i < mParticles; i++ ) {
					tmp[i] = make_float4( 0.0f, 0.0f, 0.0f, 0.0f );
				}
				NoiseValues->upload( tmp );

				data.contexts[0]->getIntegrationUtilities().initRandomNumberGenerator( integrator.getRandomNumberSeed() );
			}

			void StepKernel::ProjectionVectors( const Integrator &integrator ) {
				//check if projection vectors changed
				bool modesChanged = integrator.getProjVecChanged();

				//projection vectors changed or never allocated
				if( modesChanged || modes == NULL ) {
					int numModes = integrator.getNumProjectionVectors();

					//valid vectors?
					if( numModes == 0 ) {
						throw OpenMMException( "Projection vector size is zero." );
					}

					//if( modes != NULL && modes->_length != numModes * mParticles ) {
					if( modes != NULL && modes->getSize() != numModes * mParticles ) {
						delete modes;
						delete modeWeights;
						modes = NULL;
						modeWeights = NULL;
					}
					if( modes == NULL ) {
						/*modes = new CUDAStream<float4>( numModes * mParticles, 1, "NormalModes" );
						modeWeights = new CUDAStream<float>( numModes > data.gpu->sim.blocks ? numModes : data.gpu->sim.blocks, 1, "NormalModeWeights" );*/
						//cu->getNumThreadBlocks()*cu->ThreadBlockSize
						modes = new CudaArray( *( data.contexts[0] ), numModes * mParticles, sizeof( float4 ), "NormalModes" );
						modeWeights = new CudaArray( *( data.contexts[0] ), ( numModes > data.contexts[0]->getNumThreadBlocks()*data.contexts[0]->ThreadBlockSize ? numModes : data.contexts[0]->getNumThreadBlocks()*data.contexts[0]->ThreadBlockSize ), sizeof( float ), "NormalModeWeights" );
						oldpos = new CudaArray( *( data.contexts[0] ), data.contexts[0]->getPaddedNumAtoms(), sizeof( float4 ), "OldPositions" );
						pPosqP = new CudaArray( *( data.contexts[0] ), data.contexts[0]->getPaddedNumAtoms(), sizeof( float4 ), "MidIntegPositions" );
						modesChanged = true;
					}
					if( modesChanged ) {
						int index = 0;
						const std::vector<std::vector<Vec3> > &modeVectors = integrator.getProjectionVectors();
						std::vector<float4> tmp( numModes * mParticles );;
						for( int i = 0; i < numModes; i++ ) {
							for( int j = 0; j < mParticles; j++ ) {
								tmp[index++] = make_float4( ( float ) modeVectors[i][j][0], ( float ) modeVectors[i][j][1], ( float ) modeVectors[i][j][2], 0.0f );
							}
						}
						modes->upload( tmp );
					}
				}
			}

			void StepKernel::setOldPositions() {
				data.contexts[0]->getPosq().copyTo( *oldpos );
			}

			void StepKernel::Integrate( OpenMM::ContextImpl &context, const Integrator &integrator ) {
				ProjectionVectors( integrator );

#ifdef FAST_NOISE
				// Add noise for step
				kFastNoise( &fastmodule, data.contexts[0], integrator.getNumProjectionVectors(), ( float )( BOLTZ * integrator.getTemperature() ), iterations, *modes, *modeWeights, integrator.getMaxEigenvalue(), *NoiseValues, *pPosqP, integrator.getStepSize() );
#endif

				// Calculate Constants

				const double friction = integrator.getFriction();

				context.updateContextState();
				// Do Step
				kNMLUpdate( &updatemodule,
							data.contexts[0],
							integrator.getStepSize(),
							friction == 0.0f ? 0.0f : 1.0f / friction,
							( float )( BOLTZ * integrator.getTemperature() ),
							integrator.getNumProjectionVectors(), kIterations, *modes, *modeWeights, *NoiseValues );  // TMC setting parameters for this
			}

			void StepKernel::UpdateTime( const Integrator &integrator ) {
				data.contexts[0]->setTime( data.contexts[0]->getTime() + integrator.getStepSize() );
				data.contexts[0]->setStepCount( data.contexts[0]->getStepCount() + 1 );
				data.contexts[0]->reorderAtoms();
			}

			void StepKernel::AcceptStep( OpenMM::ContextImpl &context ) {
				kNMLAcceptMinimizationStep( &minmodule, data.contexts[0], *oldpos );
			}

			void StepKernel::RejectStep( OpenMM::ContextImpl &context ) {
				kNMLRejectMinimizationStep( &minmodule, data.contexts[0], *oldpos );
			}

			void StepKernel::LinearMinimize( OpenMM::ContextImpl &context, const Integrator &integrator, const double energy ) {
				ProjectionVectors( integrator );

				lastPE = energy;
				kNMLLinearMinimize( &linmodule, data.contexts[0], integrator.getNumProjectionVectors(), integrator.getMaxEigenvalue(), *pPosqP, *modes, *modeWeights );
			}

			double StepKernel::QuadraticMinimize( OpenMM::ContextImpl &context, const Integrator &integrator, const double energy ) {
				ProjectionVectors( integrator );

				kNMLQuadraticMinimize( &quadmodule, data.contexts[0], integrator.getMaxEigenvalue(), energy, lastPE, *pPosqP, *modeWeights, *MinimizeLambda );
				std::vector<float> tmp;
				tmp.resize( 1 );
				MinimizeLambda->download( tmp );

				return tmp[0];
			}
		}
	}
}
