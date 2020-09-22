#include "moana/renderer.hpp"

#include <cuda_runtime.h>
#include <optix_stubs.h>

#include "assert_macros.hpp"
#include "core/ptex_texture.hpp"
#include "moana/io/image.hpp"
#include "scene/texture_offsets.hpp"
#include "util/color_map.hpp"
#include "util/enumerate.hpp"

namespace moana { namespace Renderer {

struct HostBuffers {
    std::vector<float> betaBuffer;
    std::vector<float> albedoBuffer;
};

struct OutputBuffers {
    std::vector<float> cosThetaWiBuffer;
    std::vector<float> barycentricBuffer;
    std::vector<int> idBuffer;
    std::vector<float> colorBuffer;
    std::vector<float> occlusionBuffer;
    std::vector<BSDFSampleRecord> sampleRecordInBuffer;
    std::vector<BSDFSampleRecord> sampleRecordOutBuffer;
    std::vector<float> tempBuffer;
};

struct BufferManager {
    size_t depthBufferSizeInBytes;
    size_t xiBufferSizeInBytes;
    size_t cosThetaWiBufferSizeInBytes;
    size_t sampleRecordInBufferSizeInBytes;
    size_t sampleRecordOutBufferSizeInBytes;
    size_t occlusionBufferSizeInBytes;
    size_t missDirectionBufferSizeInBytes;
    size_t barycentricBufferSizeInBytes;
    size_t idBufferSizeInBytes;
    size_t colorBufferSizeInBytes;
    size_t tempBufferSizeInBytes;

    HostBuffers host;
    OutputBuffers output;
};

static void copyOutputBuffers(
    BufferManager &buffers,
    int width,
    int height,
    Params &params
) {
    buffers.output.cosThetaWiBuffer.resize(width * height * 1);
    buffers.output.barycentricBuffer.resize(width * height * 2);
    buffers.output.idBuffer.resize(width * height * 3);
    buffers.output.colorBuffer.resize(width * height * 3);
    buffers.output.occlusionBuffer.resize(width * height);
    buffers.output.sampleRecordInBuffer.resize(width * height);
    buffers.output.sampleRecordOutBuffer.resize(width * height);
    buffers.output.tempBuffer.resize(width * height * 3);

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.cosThetaWiBuffer.data()),
        params.cosThetaWiBuffer,
        buffers.cosThetaWiBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.barycentricBuffer.data()),
        params.barycentricBuffer,
        buffers.barycentricBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.idBuffer.data()),
        params.idBuffer,
        buffers.idBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.colorBuffer.data()),
        params.colorBuffer,
        buffers.colorBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.occlusionBuffer.data()),
        params.occlusionBuffer,
        buffers.occlusionBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.sampleRecordInBuffer.data()),
        params.sampleRecordInBuffer,
        buffers.sampleRecordInBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.sampleRecordOutBuffer.data()),
        params.sampleRecordOutBuffer,
        buffers.sampleRecordOutBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));

    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(buffers.output.tempBuffer.data()),
        params.tempBuffer,
        buffers.tempBufferSizeInBytes,
        cudaMemcpyDeviceToHost
    ));
}

static void resetSampleBuffers(
    BufferManager &buffers,
    int width,
    int height,
    Params &params
) {
    buffers.host.betaBuffer.resize(width * height * 3);
    std::fill(
        buffers.host.betaBuffer.begin(),
        buffers.host.betaBuffer.end(),
        1.f
    );

    buffers.host.albedoBuffer.resize(width * height * 3);
    std::fill(
        buffers.host.albedoBuffer.begin(),
        buffers.host.albedoBuffer.end(),
        0.f
    );

    std::vector<float> xiBuffer(width * height * 2, -1.f);
    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(params.xiBuffer),
        xiBuffer.data(),
        buffers.xiBufferSizeInBytes,
        cudaMemcpyHostToDevice
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.sampleRecordInBuffer),
        0,
        buffers.sampleRecordInBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.sampleRecordOutBuffer),
        0,
        buffers.sampleRecordOutBufferSizeInBytes
    ));
}

static void resetBounceBuffers(
    BufferManager &buffers,
    int width,
    int height,
    Params &params
) {
    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(params.sampleRecordInBuffer),
        reinterpret_cast<void *>(params.sampleRecordOutBuffer),
        buffers.sampleRecordOutBufferSizeInBytes,
        cudaMemcpyDeviceToDevice
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.sampleRecordOutBuffer),
        0,
        buffers.sampleRecordOutBufferSizeInBytes
    ));

    std::vector<float> depthBuffer(width * height, std::numeric_limits<float>::max());
    CHECK_CUDA(cudaMemcpy(
        reinterpret_cast<void *>(params.depthBuffer),
        depthBuffer.data(),
        buffers.depthBufferSizeInBytes,
        cudaMemcpyHostToDevice
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.cosThetaWiBuffer),
        0,
        buffers.cosThetaWiBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.occlusionBuffer),
        0,
        buffers.occlusionBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.missDirectionBuffer),
        0,
        buffers.missDirectionBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.barycentricBuffer),
        0,
        buffers.barycentricBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.idBuffer),
        0,
        buffers.idBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.colorBuffer),
        0,
        buffers.colorBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMemset(
        reinterpret_cast<void *>(params.tempBuffer),
        0,
        buffers.tempBufferSizeInBytes
    ));

}

static void mallocBuffers(
    BufferManager &buffers,
    int width,
    int height,
    Params &params
) {
    buffers.depthBufferSizeInBytes = width * height * sizeof(float);
    buffers.xiBufferSizeInBytes = width * height * 2 * sizeof(float);
    buffers.cosThetaWiBufferSizeInBytes = width * height * 1 * sizeof(float);
    buffers.sampleRecordInBufferSizeInBytes = width * height * sizeof(BSDFSampleRecord);
    buffers.sampleRecordOutBufferSizeInBytes = width * height * sizeof(BSDFSampleRecord);
    buffers.occlusionBufferSizeInBytes = width * height * 1 * sizeof(float);
    buffers.missDirectionBufferSizeInBytes = width * height * 3 * sizeof(float);
    buffers.barycentricBufferSizeInBytes = width * height * 2 * sizeof(float);
    buffers.idBufferSizeInBytes = width * height * sizeof(int) * 3;
    buffers.colorBufferSizeInBytes = width * height * sizeof(float) * 3;
    buffers.tempBufferSizeInBytes = width * height * sizeof(float) * 3;

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.depthBuffer),
        buffers.depthBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.xiBuffer),
        buffers.xiBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.cosThetaWiBuffer),
        buffers.cosThetaWiBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.sampleRecordInBuffer),
        buffers.sampleRecordInBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.sampleRecordOutBuffer),
        buffers.sampleRecordOutBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.occlusionBuffer),
        buffers.occlusionBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.missDirectionBuffer),
        buffers.missDirectionBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.barycentricBuffer),
        buffers.barycentricBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.idBuffer),
        buffers.idBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.colorBuffer),
        buffers.colorBufferSizeInBytes
    ));

    CHECK_CUDA(cudaMalloc(
        reinterpret_cast<void **>(&params.tempBuffer),
        buffers.tempBufferSizeInBytes
    ));
}

static void updateAlbedoBuffer(
    BufferManager &buffers,
    std::vector<PtexTexture> &textures,
    int width,
    int height,
    int spp,
    std::vector<float> &textureImage
) {
   ColorMap faceMap;
   ColorMap materialMap;
   std::vector<float> faceImage(width * height * 3, 0.f);
   std::vector<float> uvImage(width * height * 3, 0.f);
   for (int row = 0; row < height; row++) {
       for (int col = 0; col < width; col++) {
           const int pixelIndex = 3 * (row * width + col);

           const int idIndex = 3 * (row * width + col);
           const int primitiveID = buffers.output.idBuffer[idIndex + 0];
           const int materialID = buffers.output.idBuffer[idIndex + 1];
           const int textureIndex = buffers.output.idBuffer[idIndex + 2];
           const int faceID = primitiveID / 2;

           const int barycentricIndex = 2 * (row * width + col);
           const float alpha = buffers.output.barycentricBuffer[barycentricIndex + 0];
           const float beta = buffers.output.barycentricBuffer[barycentricIndex + 1];

           float u, v;
           if (primitiveID % 2 == 0) {
               u = alpha + beta;
               v = beta;
           } else {
               u = alpha;
               v = alpha + beta;
           }
           uvImage[pixelIndex + 0] = u;
           uvImage[pixelIndex + 1] = v;
           uvImage[pixelIndex + 2] = materialID;

           if (materialID > 0) {
               float3 color = faceMap.get(faceID);
               faceImage[pixelIndex + 0] = color.x;
               faceImage[pixelIndex + 1] = color.y;
               faceImage[pixelIndex + 2] = color.z;
           }

           float textureX = 0;
           float textureY = 0;
           float textureZ = 0;
           if (textureIndex >= 0) {
               PtexTexture texture = textures[textureIndex];

               Vec3 color = texture.lookup(
                   float2{ u, v },
                   faceID
               );
               textureX = color.x();
               textureY = color.y();
               textureZ = color.z();
           } else if (materialID > 0) {
               float3 color = materialMap.get(materialID);

               textureX = buffers.output.colorBuffer[pixelIndex + 0];
               textureY = buffers.output.colorBuffer[pixelIndex + 1];
               textureZ = buffers.output.colorBuffer[pixelIndex + 2];
           }

           buffers.host.albedoBuffer[pixelIndex + 0] = textureX;
           buffers.host.albedoBuffer[pixelIndex + 1] = textureY;
           buffers.host.albedoBuffer[pixelIndex + 2] = textureZ;

           const int cosThetaWiIndex = row * width + col;
           const float cosThetaWi = buffers.output.cosThetaWiBuffer[cosThetaWiIndex];

           textureImage[pixelIndex + 0] += (1.f / spp) * textureX * cosThetaWi;
           textureImage[pixelIndex + 1] += (1.f / spp) * textureY * cosThetaWi;
           textureImage[pixelIndex + 2] += (1.f / spp) * textureZ * cosThetaWi;
       }
   }
}

static void updateBetaBuffer(
    BufferManager &buffers,
    int width,
    int height
) {
   for (int row = 0; row < height; row++) {
       for (int col = 0; col < width; col++) {
           const int pixelIndex = 3 * (row * width + col);

           const int cosThetaWiIndex = row * width + col;
           const float cosThetaWi = buffers.output.cosThetaWiBuffer[cosThetaWiIndex];
           for (int i = 0; i < 3; i++) {
               buffers.host.betaBuffer[pixelIndex + i] *= 1.f
                   * cosThetaWi
                   * 2.f
                   * buffers.host.albedoBuffer[pixelIndex + i];
           }
       }
   }
}

static void updateEnvironmentLighting(
    SceneState &sceneState,
    BufferManager &buffers,
    int width,
    int height,
    int spp,
    Params &params,
    std::vector<float> &outputImage
) {
    // Lookup L for misses
    sceneState.arena.restoreSnapshot(sceneState.environmentState.snapshot);
    std::vector<float> environmentLightBuffer(width * height * 3, 0.f);
    EnvironmentLight::calculateEnvironmentLighting(
        width,
        height,
        sceneState.environmentState.textureObject,
        params.occlusionBuffer,
        params.missDirectionBuffer,
        environmentLightBuffer
    );

    // Calculate Li
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            const int pixelIndex = 3 * (row * width + col);
            const int environmentIndex = 3 * (row * width + col);

            for (int i = 0; i < 3; i++) {
                outputImage[pixelIndex + i] += 1.f
                    * environmentLightBuffer[environmentIndex + i]
                    * buffers.host.betaBuffer[pixelIndex + i]
                    * (1.f / spp);
            }
        }
    }
}

static void runSample(
    int sample,
    int bounces,
    OptixState &optixState,
    SceneState &sceneState,
    BufferManager &buffers,
    std::vector<PtexTexture> &textures,
    CUstream stream,
    int width,
    int height,
    int spp,
    Params &params,
    CUdeviceptr d_params,
    std::vector<float> &outputImage,
    std::vector<float> &textureImage
) {
    std::cout << "Sample #" << sample << std::endl;

    params.bounce = 0;
    params.sampleCount = sample;

    resetSampleBuffers(buffers, width, height, params);
    resetBounceBuffers(buffers, width, height, params);

    // Run intersection
    for (const auto &[i, geometry] : enumerate(sceneState.geometries)) {
        sceneState.arena.restoreSnapshot(geometry.snapshot);

        params.handle = geometry.handle;
        params.bounce = 0;
        params.rayType = 0;
        CHECK_CUDA(cudaMemcpy(
            reinterpret_cast<void *>(d_params),
            &params,
            sizeof(params),
            cudaMemcpyHostToDevice
        ));

        CHECK_OPTIX(optixLaunch(
            optixState.pipeline,
            stream,
            d_params,
            sizeof(Params),
            &optixState.sbt,
            width,
            height,
            /*depth=*/1
        ));

        CHECK_CUDA(cudaDeviceSynchronize());
    }

    // Copy buffers to host for lighting and texture calculations
    copyOutputBuffers(buffers, width, height, params);

    CHECK_CUDA(cudaDeviceSynchronize());

    // updateEnvironmentLighting(
    //     sceneState,
    //     buffers,
    //     width,
    //     height,
    //     spp,
    //     params,
    //     outputImage
    // );

    // Bounce
    for (int bounce = 0; bounce < bounces; bounce++) {
        {
            for (const auto &[j, geometry] : enumerate(sceneState.geometries)) {
                sceneState.arena.restoreSnapshot(geometry.snapshot);

                params.handle = geometry.handle;
                params.bounce = bounce;
                params.rayType = 1;
                CHECK_CUDA(cudaMemcpy(
                    reinterpret_cast<void *>(d_params),
                    &params,
                    sizeof(params),
                    cudaMemcpyHostToDevice
                ));

                CHECK_OPTIX(optixLaunch(
                    optixState.pipeline,
                    stream,
                    d_params,
                    sizeof(Params),
                    &optixState.sbt,
                    width,
                    height,
                    /*depth=*/1
                ));

                CHECK_CUDA(cudaDeviceSynchronize());
            }
            copyOutputBuffers(buffers, width, height, params);

            updateAlbedoBuffer(
                buffers,
                textures,
                width,
                height,
                spp,
                textureImage
            );

            for (int row = 0; row < height; row++) {
                for (int col = 0; col < width; col++) {
                    const int pixelIndex = 3 * (row * width + col);
                    if (buffers.output.tempBuffer[pixelIndex + 0] != 0.f) { continue; }

                    float L[3] = { 891.443777, 505.928150, 154.625939 };
                    for (int i = 0; i < 3; i++) {
                        outputImage[pixelIndex + i] += 1.f
                            * L[i]
                            * buffers.host.albedoBuffer[pixelIndex + i]
                            * buffers.host.betaBuffer[pixelIndex + i]
                            * buffers.output.tempBuffer[pixelIndex + 2]
                            * (1.f / spp);
                    }
                }
            }
        }

        updateBetaBuffer(
            buffers,
            width,
            height
        );

        resetBounceBuffers(buffers, width, height, params);

        for (const auto &[j, geometry] : enumerate(sceneState.geometries)) {
            sceneState.arena.restoreSnapshot(geometry.snapshot);

            params.handle = geometry.handle;
            params.rayType = 0;
            params.bounce = 1 + bounce;
            CHECK_CUDA(cudaMemcpy(
                reinterpret_cast<void *>(d_params),
                &params,
                sizeof(params),
                cudaMemcpyHostToDevice
            ));

            CHECK_OPTIX(optixLaunch(
                optixState.pipeline,
                stream,
                d_params,
                sizeof(Params),
                &optixState.sbt,
                width,
                height,
                /*depth=*/1
            ));

            CHECK_CUDA(cudaDeviceSynchronize());
        }

        // Copy buffers for occlusion and sample records
        copyOutputBuffers(buffers, width, height, params);
        CHECK_CUDA(cudaDeviceSynchronize());

        // updateEnvironmentLighting(
        //     sceneState,
        //     buffers,
        //     width,
        //     height,
        //     spp,
        //     params,
        //     outputImage
        // );
    }
}

void launch(
    RenderRequest renderRequest,
    OptixState &optixState,
    SceneState &sceneState,
    Cam cam,
    const std::string &exrFilename
) {
    CUdeviceptr d_params;
    CHECK_CUDA(cudaMalloc(reinterpret_cast<void **>(&d_params), sizeof(Params)));

    std::vector<PtexTexture> textures;
    for (const auto &filename : Textures::textureFilenames) {
        PtexTexture texture(MOANA_ROOT + std::string("/island/") + filename);
        textures.push_back(texture);
    }

    CUstream stream;
    CHECK_CUDA(cudaStreamCreate(&stream));

    const int width = 1024;
    const int height = 429;

    std::vector<float> outputImage(width * height * 3, 0.f);

    Params params;

    BufferManager buffers;
    mallocBuffers(buffers, width, height, params);

    Scene scene(cam);
    Camera camera = scene.getCamera(width, height);

    params.camera = camera;

    std::vector<float> textureImage(width * height * 3, 0.f);

    const int spp = renderRequest.spp;
    for (int sample = 0; sample < spp; sample++) {
        runSample(
            sample,
            renderRequest.bounces,
            optixState,
            sceneState,
            buffers,
            textures,
            stream,
            width,
            height,
            spp,
            params,
            d_params,
            outputImage,
            textureImage
        );

        int x = 1;
        while (x <= (sample + 1)) {
            if (x == (sample + 1)) {
                const std::string prefix = std::to_string(sample + 1);
                const std::string padding = std::string(6 - prefix.size(), '0');

                // fixme
                std::vector<float> adjustedImage(outputImage);
                for (int i = 0; i < width * height * 3; i++) {
                    adjustedImage[i] *= 1.f * spp / (sample + 1);
                }
                Image::save(
                    width,
                    height,
                    adjustedImage,
                    padding + prefix + "_" + exrFilename
                );
            }
            x *= 2;
        }
    }
    Image::save(
        width,
        height,
        outputImage,
        exrFilename
    );

    CHECK_CUDA(cudaFree(params.depthBuffer));
    CHECK_CUDA(cudaFree(params.xiBuffer));
    CHECK_CUDA(cudaFree(params.cosThetaWiBuffer));
    CHECK_CUDA(cudaFree(params.sampleRecordInBuffer));
    CHECK_CUDA(cudaFree(params.sampleRecordOutBuffer));
    CHECK_CUDA(cudaFree(params.missDirectionBuffer));
    CHECK_CUDA(cudaFree(params.barycentricBuffer));
    CHECK_CUDA(cudaFree(params.idBuffer));
    CHECK_CUDA(cudaFree(params.colorBuffer));

    CHECK_CUDA(cudaFree(reinterpret_cast<void *>(d_params)));
}

} }
