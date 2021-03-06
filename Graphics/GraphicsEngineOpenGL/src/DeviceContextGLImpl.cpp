/*     Copyright 2015-2018 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include "pch.h"
#include <iostream>
#include <fstream>
#include <string>

#include "SwapChainGL.h"
#include "DeviceContextGLImpl.h"
#include "RenderDeviceGLImpl.h"
#include "GLTypeConversions.h"

#include "BufferGLImpl.h"
#include "ShaderGLImpl.h"
#include "VAOCache.h"
#include "Texture1D_OGL.h"
#include "Texture1DArray_OGL.h"
#include "Texture2D_OGL.h"
#include "Texture2DArray_OGL.h"
#include "Texture3D_OGL.h"
#include "SamplerGLImpl.h"
#include "GraphicsAccessories.h"
#include "BufferViewGLImpl.h"
#include "PipelineStateGLImpl.h"
#include "ShaderResourceBindingGLImpl.h"

using namespace std;

namespace Diligent
{
    DeviceContextGLImpl::DeviceContextGLImpl( IReferenceCounters *pRefCounters, class RenderDeviceGLImpl *pDeviceGL, bool bIsDeferred ) : 
        TDeviceContextBase(pRefCounters, pDeviceGL, bIsDeferred),
        m_ContextState(pDeviceGL),
        m_CommitedResourcesTentativeBarriers(0),
        m_DefaultFBO(false)
    {
        m_BoundWritableTextures.reserve( 16 );
        m_BoundWritableBuffers.reserve( 16 );
    }

    IMPLEMENT_QUERY_INTERFACE( DeviceContextGLImpl, IID_DeviceContextGL, TDeviceContextBase )


    void DeviceContextGLImpl::SetPipelineState(IPipelineState *pPipelineState)
    {
        TDeviceContextBase::SetPipelineState(pPipelineState);

        const auto &Desc = pPipelineState->GetDesc();
        if (Desc.IsComputePipeline)
        {
        }
        else
        {
            const auto& GraphicsPipeline = Desc.GraphicsPipeline;
            // Set rasterizer state
            {
                const auto &RasterizerDesc = GraphicsPipeline.RasterizerDesc;

                m_ContextState.SetFillMode(RasterizerDesc.FillMode);
                m_ContextState.SetCullMode(RasterizerDesc.CullMode);
                m_ContextState.SetFrontFace(RasterizerDesc.FrontCounterClockwise);
                m_ContextState.SetDepthBias( static_cast<Float32>( RasterizerDesc.DepthBias ), RasterizerDesc.SlopeScaledDepthBias );
                if( RasterizerDesc.DepthBiasClamp != 0 )
                    LOG_WARNING_MESSAGE( "Depth bias clamp is not supported on OpenGL" );
                m_ContextState.SetDepthClamp( RasterizerDesc.DepthClipEnable );
                m_ContextState.EnableScissorTest( RasterizerDesc.ScissorEnable );
                if( RasterizerDesc.AntialiasedLineEnable )
                    LOG_WARNING_MESSAGE( "Line antialiasing is not supported on OpenGL" );
            }

            // Set blend state
            {
                const auto &BSDsc = GraphicsPipeline.BlendDesc;
                m_ContextState.SetBlendState(BSDsc, GraphicsPipeline.SampleMask);
            }

            // Set depth-stencil state
            {
                const auto &DepthStencilDesc = GraphicsPipeline.DepthStencilDesc;

                m_ContextState.EnableDepthTest( DepthStencilDesc.DepthEnable );
                m_ContextState.EnableDepthWrites( DepthStencilDesc.DepthWriteEnable );
                m_ContextState.SetDepthFunc( DepthStencilDesc.DepthFunc );

                m_ContextState.EnableStencilTest( DepthStencilDesc.StencilEnable );

                m_ContextState.SetStencilWriteMask( DepthStencilDesc.StencilWriteMask );

                {
                    const auto &FrontFace = DepthStencilDesc.FrontFace;
                    m_ContextState.SetStencilFunc( GL_FRONT, FrontFace.StencilFunc, m_StencilRef, DepthStencilDesc.StencilReadMask );
                    m_ContextState.SetStencilOp( GL_FRONT, FrontFace.StencilFailOp, FrontFace.StencilDepthFailOp, FrontFace.StencilPassOp );
                }

                {
                    const auto &BackFace = DepthStencilDesc.BackFace;
                    m_ContextState.SetStencilFunc( GL_BACK, BackFace.StencilFunc, m_StencilRef, DepthStencilDesc.StencilReadMask );
                    m_ContextState.SetStencilOp( GL_BACK, BackFace.StencilFailOp, BackFace.StencilDepthFailOp, BackFace.StencilPassOp );
                }
            }
            m_bVAOIsUpToDate = false;
        }
    }

    void DeviceContextGLImpl::TransitionShaderResources(IPipelineState *pPipelineState, IShaderResourceBinding *pShaderResourceBinding)
    {

    }

    void DeviceContextGLImpl::CommitShaderResources(IShaderResourceBinding *pShaderResourceBinding, Uint32 Flags)
    {
        if(!DeviceContextBase::CommitShaderResources<PipelineStateGLImpl>(pShaderResourceBinding, Flags, 0))
            return;

        if(m_CommitedResourcesTentativeBarriers != 0)
            LOG_INFO_MESSAGE("Not all tentative resource barriers have been executed since the last call to CommitShaderResources(). Did you forget to call Draw()/DispatchCompute() ?");

        m_CommitedResourcesTentativeBarriers = 0;
        BindProgramResources( m_CommitedResourcesTentativeBarriers, pShaderResourceBinding );
        // m_CommitedResourcesTentativeBarriers will contain memory barriers that will be required 
        // AFTER the actual draw/dispatch command is executed. Before that they have no meaning
    }

    void DeviceContextGLImpl::SetStencilRef(Uint32 StencilRef)
    {
        if (TDeviceContextBase::SetStencilRef(StencilRef, 0))
        {
            m_ContextState.SetStencilRef(GL_FRONT, StencilRef);
            m_ContextState.SetStencilRef(GL_BACK, StencilRef);
        }
    }

    void DeviceContextGLImpl::SetBlendFactors(const float* pBlendFactors)
    {
        if (TDeviceContextBase::SetBlendFactors(pBlendFactors, 0))
        {
            m_ContextState.SetBlendFactors(m_BlendFactors);
        }
    }

    void DeviceContextGLImpl::SetVertexBuffers( Uint32 StartSlot, Uint32 NumBuffersSet, IBuffer **ppBuffers, Uint32 *pStrides, Uint32 *pOffsets, Uint32 Flags )
    {
        TDeviceContextBase::SetVertexBuffers( StartSlot, NumBuffersSet, ppBuffers, pStrides, pOffsets, Flags );
        m_bVAOIsUpToDate = false;
    }

    void DeviceContextGLImpl::InvalidateState()
    {
        TDeviceContextBase::InvalidateState();

        m_ContextState.Invalidate();
        m_BoundWritableTextures.clear();
        m_BoundWritableBuffers.clear();
        m_bVAOIsUpToDate = false;
    }

    void DeviceContextGLImpl::SetIndexBuffer( IBuffer *pIndexBuffer, Uint32 ByteOffset )
    {
        TDeviceContextBase::SetIndexBuffer( pIndexBuffer, ByteOffset );
        m_bVAOIsUpToDate = false;
    }

    void DeviceContextGLImpl::SetViewports( Uint32 NumViewports, const Viewport *pViewports, Uint32 RTWidth, Uint32 RTHeight  )
    {
        TDeviceContextBase::SetViewports( NumViewports, pViewports, RTWidth, RTHeight  );

        VERIFY( NumViewports == m_NumViewports, "Unexpected number of viewports" );
        if( NumViewports == 1 )
        {
            const auto &vp = m_Viewports[0];
            // Note that OpenGL and DirectX use different origin of 
            // the viewport in window coordinates:
            //
            // DirectX (0,0)
            //     \ ____________
            //      |            |
            //      |            |
            //      |            |
            //      |            |
            //      |____________|
            //     /
            //  OpenGL (0,0)
            //
            float BottomLeftY = static_cast<float>(RTHeight) - (vp.TopLeftY + vp.Height);
            float BottomLeftX = vp.TopLeftX;

            Int32 x = static_cast<int>(BottomLeftX);
            Int32 y = static_cast<int>(BottomLeftY);
            Int32 w = static_cast<int>(vp.Width);
            Int32 h = static_cast<int>(vp.Height);
            if( static_cast<float>(x) == BottomLeftX &&
                static_cast<float>(y) == BottomLeftY &&
                static_cast<float>(w) == vp.Width &&
                static_cast<float>(h) == vp.Height )
            {
                glViewport( x, y, w, h );
            }
            else
            {
                glViewportIndexedf( 0, BottomLeftX, BottomLeftY, vp.Width, vp.Height );
            }
            CHECK_GL_ERROR( "Failed to set viewport" );

            glDepthRangef( vp.MinDepth, vp.MaxDepth );
            CHECK_GL_ERROR( "Failed to set depth range" );
        }
        else
        {
            for( Uint32 i = 0; i < NumViewports; ++i )
            {
                const auto &vp = m_Viewports[i];
                float BottomLeftY = static_cast<float>(RTHeight) - (vp.TopLeftY + vp.Height);
                float BottomLeftX = vp.TopLeftX;
                glViewportIndexedf( i, BottomLeftX, BottomLeftY, vp.Width, vp.Height );
                CHECK_GL_ERROR( "Failed to set viewport #", i );
                glDepthRangef( vp.MinDepth, vp.MaxDepth );
                CHECK_GL_ERROR( "Failed to set depth range for viewport #", i );
            }
        }
    }

    void DeviceContextGLImpl::SetScissorRects( Uint32 NumRects, const Rect *pRects, Uint32 RTWidth, Uint32 RTHeight  )
    {
        TDeviceContextBase::SetScissorRects(NumRects, pRects, RTWidth, RTHeight);

        VERIFY( NumRects == m_NumScissorRects, "Unexpected number of scissor rects" );
        if( NumRects == 1 )
        {
            const auto &Rect = m_ScissorRects[0];
            // Note that OpenGL and DirectX use different origin 
            // of the viewport in window coordinates:
            //
            // DirectX (0,0)
            //     \ ____________
            //      |            |
            //      |            |
            //      |            |
            //      |            |
            //      |____________|
            //     /
            //  OpenGL (0,0)
            //
            auto glBottom = RTHeight - Rect.bottom;

            auto width  = Rect.right - Rect.left;
            auto height = Rect.bottom - Rect.top;
            glScissor( Rect.left, glBottom, width, height );
            CHECK_GL_ERROR( "Failed to set scissor rect" );
        }
        else
        {
            for( Uint32 sr = 0; sr < NumRects; ++sr )
            {
                const auto &Rect = m_ScissorRects[sr];
                auto glBottom = RTHeight - Rect.bottom;
                auto width  = Rect.right - Rect.left;
                auto height = Rect.bottom - Rect.top;
                glScissorIndexed(sr, Rect.left, glBottom, width, height );
                CHECK_GL_ERROR( "Failed to set scissor rect #", sr );
            }
        }
    }

    void DeviceContextGLImpl::CommitRenderTargets()
    {
        if (m_IsDefaultFramebufferBound)
        {
            auto *pSwapChainGL = ValidatedCast<ISwapChainGL>(m_pSwapChain.RawPtr());
            GLuint DefaultFBOHandle = pSwapChainGL->GetDefaultFBO();
            if (m_DefaultFBO != DefaultFBOHandle)
            {
                m_DefaultFBO = GLObjectWrappers::GLFrameBufferObj(true, GLObjectWrappers::GLFBOCreateReleaseHelper(DefaultFBOHandle));
            }
            m_ContextState.BindFBO(m_DefaultFBO);
        }
        else
        {
            VERIFY(m_NumBoundRenderTargets != 0 || m_pBoundDepthStencil, "At least one render target or a depth stencil is expected");

            Uint32 NumRenderTargets = m_NumBoundRenderTargets;
            VERIFY(NumRenderTargets < MaxRenderTargets, "Too many render targets (", NumRenderTargets, ") are being set");
            NumRenderTargets = std::min(NumRenderTargets, MaxRenderTargets);

            const auto& CtxCaps = m_ContextState.GetContextCaps();
            VERIFY(NumRenderTargets < static_cast<Uint32>(CtxCaps.m_iMaxDrawBuffers), "This device only supports ", CtxCaps.m_iMaxDrawBuffers, " draw buffers, but ", NumRenderTargets, " are being set");
            NumRenderTargets = std::min(NumRenderTargets, static_cast<Uint32>(CtxCaps.m_iMaxDrawBuffers));

            ITextureView *pBoundRTVs[MaxRenderTargets] = {};
            for (Uint32 rt = 0; rt < NumRenderTargets; ++rt)
                pBoundRTVs[rt] = m_pBoundRenderTargets[rt];

            auto *pRenderDeviceGL = ValidatedCast<RenderDeviceGLImpl>(m_pDevice.RawPtr());
            auto CurrentNativeGLContext = m_ContextState.GetCurrentGLContext();
            auto &FBOCache = pRenderDeviceGL->GetFBOCache(CurrentNativeGLContext);
            const auto& FBO = FBOCache.GetFBO(NumRenderTargets, pBoundRTVs, m_pBoundDepthStencil, m_ContextState);
            // Even though the write mask only applies to writes to a framebuffer, the mask state is NOT 
            // Framebuffer state. So it is NOT part of a Framebuffer Object or the Default Framebuffer. 
            // Binding a new framebuffer will NOT affect the mask.
            m_ContextState.BindFBO(FBO);
        }
        // Set the viewport to match the render target size
        SetViewports(1, nullptr, 0, 0);
    }

    void DeviceContextGLImpl::SetRenderTargets( Uint32 NumRenderTargets, ITextureView *ppRenderTargets[], ITextureView *pDepthStencil )
    {
        if( TDeviceContextBase::SetRenderTargets( NumRenderTargets, ppRenderTargets, pDepthStencil ) )
            CommitRenderTargets();
    }

    void DeviceContextGLImpl::BindProgramResources( Uint32 &NewMemoryBarriers, IShaderResourceBinding *pResBinding )
    {
        auto *pRenderDeviceGL = ValidatedCast<RenderDeviceGLImpl>(m_pDevice.RawPtr());
        if (!m_pPipelineState)
        {
            LOG_ERROR("No pipeline state is bound");
            return;
        }
        auto *pPipelineStateGL = ValidatedCast<PipelineStateGLImpl>(m_pPipelineState.RawPtr());
        auto *pShaderResBindingGL = ValidatedCast<ShaderResourceBindingGLImpl>(pResBinding);

        const auto &DeviceCaps = pRenderDeviceGL->GetDeviceCaps();
        auto &Prog = pPipelineStateGL->GetGLProgram();
        auto &Pipeline = pPipelineStateGL->GetGLProgramPipeline( m_ContextState.GetCurrentGLContext() );
        VERIFY( Prog ^ Pipeline, "Only one of program or pipeline can be specified" );
        if( !(Prog || Pipeline) )
        {
            LOG_ERROR_MESSAGE("No program/program pipeline is set for the draw call");
            return;
        }
        auto ProgramPipelineSupported = DeviceCaps.bSeparableProgramSupported;

        // WARNING: glUseProgram() overrides glBindProgramPipeline(). That is, if you have a program in use and
        // a program pipeline bound, all rendering will use the program that is in use, not the pipeline programs!!!
        // So make sure that glUseProgram(0) has been called if pipeline is in use
        m_ContextState.SetProgram( Prog );
        if( ProgramPipelineSupported )
            m_ContextState.SetPipeline( Pipeline );

        size_t NumPrograms = ProgramPipelineSupported ? pPipelineStateGL->GetNumShaders() : 1;
        GLuint UniformBuffBindPoint = 0;
        GLuint TextureIndex = 0;
        m_BoundWritableTextures.clear();
        m_BoundWritableBuffers.clear();
        for( size_t ProgNum = 0; ProgNum < NumPrograms; ++ProgNum )
        {
            auto *pShaderGL = static_cast<ShaderGLImpl*>(pPipelineStateGL->GetShaders()[ProgNum]);
            auto &GLProgramObj = ProgramPipelineSupported ? pShaderGL->m_GlProgObj : Prog;

            GLProgramResources *pDynamicResources = pShaderResBindingGL ? &pShaderResBindingGL->GetProgramResources(pShaderGL->GetDesc().ShaderType, pPipelineStateGL) : nullptr;
#ifdef VERIFY_RESOURCE_BINDINGS
            GLProgramObj.dbgVerifyBindingCompleteness(pDynamicResources, pPipelineStateGL);
#endif

            for(int BindDynamicResources = 0; BindDynamicResources < (pShaderResBindingGL ? 2 : 1); ++BindDynamicResources)
            {
                GLProgramResources &ProgResources = BindDynamicResources ? *pDynamicResources : GLProgramObj.GetConstantResources();

#ifdef VERIFY_RESOURCE_BINDINGS
                ProgResources.dbgVerifyResourceBindings();
#endif
                
                GLuint GLProgID = GLProgramObj;
                auto &UniformBlocks = ProgResources.GetUniformBlocks();
                for( auto it = UniformBlocks.begin(); it != UniformBlocks.end(); ++it )
                {
                    for(Uint32 ArrInd = 0; ArrInd < it->pResources.size(); ++ArrInd)
                    {
                        auto& Resource = it->pResources[ArrInd];
                        if (Resource)
                        {
                            auto *pBufferOGL = ValidatedCast<BufferGLImpl>(Resource.RawPtr());
                            pBufferOGL->BufferMemoryBarrier(
                                GL_UNIFORM_BARRIER_BIT,// Shader uniforms sourced from buffer objects after the barrier 
                                                       // will reflect data written by shaders prior to the barrier
                                m_ContextState);

                            glBindBufferBase(GL_UNIFORM_BUFFER, UniformBuffBindPoint, pBufferOGL->m_GlBuffer);
                            CHECK_GL_ERROR("Failed to bind uniform buffer");
                            //glBindBufferRange(GL_UNIFORM_BUFFER, it->Index, pBufferOGL->m_GlBuffer, 0, pBufferOGL->GetDesc().uiSizeInBytes);

                            glUniformBlockBinding(GLProgID, it->Index + ArrInd, UniformBuffBindPoint);
                            CHECK_GL_ERROR("glUniformBlockBinding() failed");

                            ++UniformBuffBindPoint;
                        }
                        else
                        {
#define LOG_MISSING_BINDING(VarType, Res, ArrInd)\
                            do{                                      \
                                if(Res->pResources.size()>1)         \
                                    LOG_ERROR_MESSAGE( "No ", VarType, " is bound to \"", Res->Name, '[', ArrInd, "]\" variable in shader \"", pShaderGL->GetDesc().Name, "\"" );\
                                else                                 \
                                    LOG_ERROR_MESSAGE( "No ", VarType, " is bound to \"", Res->Name, "\" variable in shader \"", pShaderGL->GetDesc().Name, "\"" );\
                            }while(false)

                            LOG_MISSING_BINDING("uniform buffer", it, ArrInd);
                        }
                    }
                }

                auto &Samplers = ProgResources.GetSamplers();
                for( auto it = Samplers.begin(); it != Samplers.end(); ++it )
                {
                    for(Uint32 ArrInd = 0; ArrInd < it->pResources.size(); ++ArrInd)
                    {
                        auto &Resource = it->pResources[ArrInd];
                        if( Resource )
                        {
                            if( it->Type == GL_SAMPLER_BUFFER ||
                                it->Type == GL_INT_SAMPLER_BUFFER ||
                                it->Type == GL_UNSIGNED_INT_SAMPLER_BUFFER )
                            {
                                auto *pBufViewOGL = ValidatedCast<BufferViewGLImpl>(Resource.RawPtr());
                                auto *pBuffer = pBufViewOGL->GetBuffer();

                                m_ContextState.BindTexture( TextureIndex, GL_TEXTURE_BUFFER, pBufViewOGL->GetTexBufferHandle() );
                                m_ContextState.BindSampler( TextureIndex, GLObjectWrappers::GLSamplerObj(false) ); // Use default texture sampling parameters

                                CHECK_DYNAMIC_TYPE( BufferGLImpl, pBuffer );
                                static_cast<BufferGLImpl*>(pBuffer)->BufferMemoryBarrier(
                                    GL_TEXTURE_FETCH_BARRIER_BIT, // Texture fetches from shaders, including fetches from buffer object 
                                                                  // memory via buffer textures, after the barrier will reflect data 
                                                                  // written by shaders prior to the barrier
                                    m_ContextState);
                            }
                            else
                            {
                                auto *pTexViewOGL = ValidatedCast<TextureViewGLImpl>(Resource.RawPtr());
                                m_ContextState.BindTexture( TextureIndex, pTexViewOGL->GetBindTarget(), pTexViewOGL->GetHandle() );

                                auto *pTexture = pTexViewOGL->GetTexture();
                                CHECK_DYNAMIC_TYPE( TextureBaseGL, pTexture );
                                static_cast<TextureBaseGL*>(pTexture)->TextureMemoryBarrier(
                                    GL_TEXTURE_FETCH_BARRIER_BIT, // Texture fetches from shaders, including fetches from buffer object 
                                                                  // memory via buffer textures, after the barrier will reflect data 
                                                                  // written by shaders prior to the barrier
                                    m_ContextState);

                                SamplerGLImpl *pSamplerGL = nullptr;
                                if (it->pStaticSampler)
                                {
                                    pSamplerGL = it->pStaticSampler;
                                }
                                else
                                {
                                    auto pSampler = pTexViewOGL->GetSampler();
                                    pSamplerGL = ValidatedCast<SamplerGLImpl>( pSampler );
                                }
                            
                                if( pSamplerGL )
                                {
                                    m_ContextState.BindSampler( TextureIndex, pSamplerGL->GetHandle() );
                                }
                            }

                            // Texture is now bound to texture slot TextureIndex.
                            // We now need to set the program uniform to use that slot
                            if( ProgramPipelineSupported )
                            {
                                // glProgramUniform1i does not require program to be bound to the pipeline
                                glProgramUniform1i( GLProgramObj, it->Location + ArrInd, TextureIndex );
                            }
                            else
                            {
                                // glUniform1i requires program to be bound to the pipeline
                                glUniform1i( it->Location + ArrInd, TextureIndex );
                            }
                            CHECK_GL_ERROR( "Failed to bind sampler uniform to texture slot" );

                            ++TextureIndex;
                        }
                        else
                        {
                            LOG_MISSING_BINDING("texture sampler", it, ArrInd);
                        }
                    }
                }

#if GL_ARB_shader_image_load_store
                auto &Images = ProgResources.GetImages();
                for( auto it = Images.begin(); it != Images.end(); ++it )
                {
                    for(Uint32 ArrInd = 0; ArrInd < it->pResources.size(); ++ArrInd)
                    {
                        auto &Resource = it->pResources[ArrInd];
                        if( Resource )
                        {
                            auto *pTexViewOGL = ValidatedCast<TextureViewGLImpl>(Resource.RawPtr());
                            const auto &ViewDesc = pTexViewOGL->GetDesc();

                            if( ViewDesc.AccessFlags & UAV_ACCESS_FLAG_WRITE )
                            {
                                auto *pTex = pTexViewOGL->GetTexture();
                                CHECK_DYNAMIC_TYPE( TextureBaseGL, pTex );
                                auto *pTexGL = static_cast<TextureBaseGL*>(pTex);

                                pTexGL->TextureMemoryBarrier(
                                    GL_SHADER_IMAGE_ACCESS_BARRIER_BIT,// Memory accesses using shader image load, store, and atomic built-in 
                                                                       // functions issued after the barrier will reflect data written by shaders 
                                                                       // prior to the barrier. Additionally, image stores and atomics issued after 
                                                                       // the barrier will not execute until all memory accesses (e.g., loads, 
                                                                       // stores, texture fetches, vertex fetches) initiated prior to the barrier 
                                                                       // complete.
                                    m_ContextState);
                                // We cannot set pending memory barriers here, because
                                // if some texture is bound twice, the logic will fail
                                m_BoundWritableTextures.push_back( pTexGL );
                            }

        #ifdef _DEBUG
                            // Check that the texure being bound has immutable storage
                            {
                                m_ContextState.BindTexture( -1, pTexViewOGL->GetBindTarget(), pTexViewOGL->GetHandle() );
                                GLint IsImmutable = 0;
                                glGetTexParameteriv( pTexViewOGL->GetBindTarget(), GL_TEXTURE_IMMUTABLE_FORMAT, &IsImmutable );
                                CHECK_GL_ERROR( "glGetTexParameteriv() failed" );
                                VERIFY( IsImmutable, "Only immutable textures can be bound to pipeline using glBindImageTexture()" );
                                m_ContextState.BindTexture( -1, pTexViewOGL->GetBindTarget(), GLObjectWrappers::GLTextureObj(false) );
                            }
        #endif
                            auto GlTexFormat = TexFormatToGLInternalTexFormat( ViewDesc.Format );
                            // Note that if a format qulifier is specified in the shader, the format
                            // must match it

                            GLboolean Layered = ViewDesc.NumArraySlices > 1 && ViewDesc.FirstArraySlice == 0;
                            // If "layered" is TRUE, the entire Mip level is bound. Layer parameter is ignored in this
                            // case. If "layered" is FALSE, only the single layer identified by "layer" will
                            // be bound. When "layered" is FALSE, the single bound layer is treated as a 2D texture.
                            GLint Layer = ViewDesc.FirstArraySlice;

                            auto GLAccess = AccessFlags2GLAccess( ViewDesc.AccessFlags );
                            // WARNING: Texture being bound to the image unit must be complete
                            // That means that if an integer texture is being bound, its 
                            // GL_TEXTURE_MIN_FILTER and GL_TEXTURE_MAG_FILTER must be NEAREST,
                            // otherwise it will be incomplete
                            m_ContextState.BindImage( it->BindingPoint + ArrInd, pTexViewOGL, ViewDesc.MostDetailedMip, Layered, Layer, GLAccess, GlTexFormat );
                        }
                        else
                        {
                            LOG_MISSING_BINDING("image", it, ArrInd);
                        }
                    }
                }
#endif

#if GL_ARB_shader_storage_buffer_object
                auto &StorageBlocks = ProgResources.GetStorageBlocks();
                for( auto it = StorageBlocks.begin(); it != StorageBlocks.end(); ++it )
                {
                    for(Uint32 ArrInd = 0; ArrInd < it->pResources.size(); ++ArrInd)
                    {
                        auto &Resource = it->pResources[ArrInd];
                        if( Resource )
                        {
                            auto *pBufferViewOGL = ValidatedCast<BufferViewGLImpl>(Resource.RawPtr());
                            const auto &ViewDesc = pBufferViewOGL->GetDesc();
                            VERIFY( ViewDesc.ViewType == BUFFER_VIEW_UNORDERED_ACCESS || ViewDesc.ViewType == BUFFER_VIEW_SHADER_RESOURCE, "Unexpceted buffer view type" );

                            auto *pBuffer = pBufferViewOGL->GetBuffer();
                            CHECK_DYNAMIC_TYPE( BufferGLImpl, pBuffer );
                            auto *pBufferOGL = static_cast<BufferGLImpl*>(pBuffer);

                            pBufferOGL->BufferMemoryBarrier(
                                GL_SHADER_STORAGE_BARRIER_BIT,// Accesses to shader storage blocks after the barrier 
                                                              // will reflect writes prior to the barrier
                                m_ContextState);

                            glBindBufferRange( GL_SHADER_STORAGE_BUFFER, it->Binding + ArrInd, pBufferOGL->m_GlBuffer, ViewDesc.ByteOffset, ViewDesc.ByteWidth );
                            CHECK_GL_ERROR( "Failed to bind shader storage buffer" );

                            if( ViewDesc.ViewType == BUFFER_VIEW_UNORDERED_ACCESS )
                                m_BoundWritableBuffers.push_back( pBufferOGL );
                        }
                        else
                        {
                            LOG_MISSING_BINDING("shader storage block", it, ArrInd);
                        }
                    }
                }
#endif
            }
        }

#if GL_ARB_shader_image_load_store
        // Go through the list of textures bound as AUVs and set the required memory barriers
        for( auto pWritableTex = m_BoundWritableTextures.begin(); pWritableTex != m_BoundWritableTextures.end(); ++pWritableTex )
        {
            Uint32 TextureMemBarriers =
                GL_TEXTURE_UPDATE_BARRIER_BIT // Writes to a texture via glTex(Sub)Image*, glCopyTex(Sub)Image*, 
                                              // glClearTex*Image, glCompressedTex(Sub)Image*, and reads via
                                              // glGetTexImage() after the barrier will reflect data written by 
                                              // shaders prior to the barrier

                | GL_TEXTURE_FETCH_BARRIER_BIT  // Texture fetches from shaders, including fetches from buffer object 
                                                // memory via buffer textures, after the barrier will reflect data 
                                                // written by shaders prior to the barrier

                | GL_PIXEL_BUFFER_BARRIER_BIT // Reads and writes of buffer objects via the GL_PIXEL_PACK_BUFFER and
                                              // GL_PIXEL_UNPACK_BUFFER bidnings after the barrier will reflect data 
                                              // written by shaders prior to the barrier

                | GL_FRAMEBUFFER_BARRIER_BIT // Reads and writes via framebuffer object attachments after the 
                                             // barrier will reflect data written by shaders prior to the barrier. 
                                             // Additionally, framebuffer writes issued after the barrier will wait 
                                             // on the completion of all shader writes issued prior to the barrier.

                | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;

            NewMemoryBarriers |= TextureMemBarriers;

            // Set new required barriers for the time when texture is used next time
            (*pWritableTex)->SetPendingMemoryBarriers( TextureMemBarriers );
        }

        for( auto pWritableBuff = m_BoundWritableBuffers.begin(); pWritableBuff != m_BoundWritableBuffers.end(); ++pWritableBuff )
        {
            Uint32 BufferMemoryBarriers =
                GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT |
                GL_ELEMENT_ARRAY_BARRIER_BIT |
                GL_UNIFORM_BARRIER_BIT |
                GL_COMMAND_BARRIER_BIT | 
                GL_BUFFER_UPDATE_BARRIER_BIT |
                GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT |
                GL_SHADER_STORAGE_BARRIER_BIT |
                GL_TEXTURE_FETCH_BARRIER_BIT;

            NewMemoryBarriers |= BufferMemoryBarriers;
            // Set new required barriers for the time when buffer is used next time
            (*pWritableBuff)->SetPendingMemoryBarriers( BufferMemoryBarriers );
        }
#endif
    }

    void DeviceContextGLImpl::Draw( DrawAttribs &DrawAttribs )
    {
        if( DrawAttribs.Topology == PRIMITIVE_TOPOLOGY_UNDEFINED )
        {
            LOG_ERROR_MESSAGE("Primitive topology is undefined");
            return;
        }

        if (!m_pPipelineState)
        {
            LOG_ERROR("No pipeline state is bound.");
            return;
        }

        auto *pRenderDeviceGL = ValidatedCast<RenderDeviceGLImpl>(m_pDevice.RawPtr());
        auto CurrNativeGLContext = pRenderDeviceGL->m_GLContext.GetCurrentNativeGLContext();
        if(!m_bVAOIsUpToDate)
        {
            auto &VAOCache = pRenderDeviceGL->GetVAOCache(CurrNativeGLContext);
            IBuffer *pIndexBuffer = DrawAttribs.IsIndexed ? m_pIndexBuffer.RawPtr() : nullptr;
            if( m_pPipelineState->GetDesc().GraphicsPipeline.InputLayout.NumElements > 0 || pIndexBuffer != nullptr)
            {
                const auto& VAO = VAOCache.GetVAO( m_pPipelineState, pIndexBuffer, m_VertexStreams, m_NumVertexStreams, m_ContextState );
                m_ContextState.BindVAO( VAO );
            }
            else
            {
                // Draw command will fail if no VAO is bound. If no vertex description is set
                // (which is the case if, for instance, the command only inputs VertexID),
                // use empty VAO
                const auto& VAO = VAOCache.GetEmptyVAO();
                m_ContextState.BindVAO( VAO );
            }
            m_bVAOIsUpToDate = true;
        }

        GLenum GlTopology;
        if (DrawAttribs.Topology >= PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
        {
#if GL_ARB_tessellation_shader
            GlTopology = GL_PATCHES;
            auto NumVertices = static_cast<Int32>(DrawAttribs.Topology - PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
            m_ContextState.SetNumPatchVertices(NumVertices);
#else
            UNSUPPORTED("Tessellation is not supported");
#endif
        }
        else
        {
            GlTopology = PrimitiveTopologyToGLTopology( DrawAttribs.Topology );
        }
        GLenum IndexType = 0;
        Uint32 FirstIndexByteOffset = 0;
        if( DrawAttribs.IsIndexed )
        {
            IndexType = TypeToGLType( DrawAttribs.IndexType );
            VERIFY( IndexType == GL_UNSIGNED_BYTE || IndexType == GL_UNSIGNED_SHORT || IndexType == GL_UNSIGNED_INT,
                    "Unsupported index type" );
            VERIFY( m_pIndexBuffer, "Index Buffer is not bound to the pipeline" );
            FirstIndexByteOffset = static_cast<Uint32>(GetValueSize( DrawAttribs.IndexType )) * DrawAttribs.FirstIndexLocation + m_IndexDataStartOffset;
        }

        // NOTE: Base Vertex and Base Instance versions are not supported even in OpenGL ES 3.1
        // This functionality can be emulated by adjusting stream offsets. This, however may cause
        // errors in case instance data is read from the same stream as vertex data. Thus handling
        // such cases is left to the application

        // http://www.opengl.org/wiki/Vertex_Rendering
        if( DrawAttribs.IsIndirect )
        {
#if GL_ARB_draw_indirect
            // The indirect rendering functions take their data from the buffer currently bound to the 
            // GL_DRAW_INDIRECT_BUFFER binding. Thus, any of indirect draw functions will fail if no buffer is 
            // bound to that binding.
            VERIFY( DrawAttribs.pIndirectDrawAttribs, "Indirect draw command attributes buffer is not set" );
            if( DrawAttribs.pIndirectDrawAttribs )
            {
                auto *pBufferOGL = static_cast<BufferGLImpl*>(DrawAttribs.pIndirectDrawAttribs);

                pBufferOGL->BufferMemoryBarrier(
                    GL_COMMAND_BARRIER_BIT,// Command data sourced from buffer objects by
                                           // Draw*Indirect and DispatchComputeIndirect commands after the barrier
                                           // will reflect data written by shaders prior to the barrier.The buffer 
                                           // objects affected by this bit are derived from the DRAW_INDIRECT_BUFFER 
                                           // and DISPATCH_INDIRECT_BUFFER bindings.
                    m_ContextState);

                glBindBuffer( GL_DRAW_INDIRECT_BUFFER, pBufferOGL->m_GlBuffer );
            }

            if( DrawAttribs.IsIndexed )
            {
                //typedef  struct {
                //    GLuint  count;
                //    GLuint  instanceCount;
                //    GLuint  firstIndex;
                //    GLuint  baseVertex;
                //    GLuint  baseInstance;
                //} DrawElementsIndirectCommand;
                glDrawElementsIndirect( GlTopology, IndexType, reinterpret_cast<const void*>( static_cast<size_t>(DrawAttribs.IndirectDrawArgsOffset) ) );
                // Note that on GLES 3.1, baseInstance is present but reserved and must be zero
                CHECK_GL_ERROR( "glDrawElementsIndirect() failed" );
            }
            else
            {
                //typedef  struct {
                //   GLuint  count;
                //   GLuint  instanceCount;
                //   GLuint  first;
                //   GLuint  baseInstance;
                //} DrawArraysIndirectCommand;
                glDrawArraysIndirect( GlTopology, reinterpret_cast<const void*>( static_cast<size_t>(DrawAttribs.IndirectDrawArgsOffset) ) );
                // Note that on GLES 3.1, baseInstance is present but reserved and must be zero
                CHECK_GL_ERROR( "glDrawArraysIndirect() failed" );
            }

            glBindBuffer( GL_DRAW_INDIRECT_BUFFER, 0 );
#else
            UNSUPPORTED("Indirect rendering is not supported");
#endif
        }
        else
        {
            if( DrawAttribs.NumInstances > 1 )
            {
                if( DrawAttribs.IsIndexed )
                {
                    if( DrawAttribs.BaseVertex )
                    {
                        if( DrawAttribs.FirstInstanceLocation )
                            glDrawElementsInstancedBaseVertexBaseInstance( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ), DrawAttribs.NumInstances, DrawAttribs.BaseVertex, DrawAttribs.FirstInstanceLocation );
                        else
                            glDrawElementsInstancedBaseVertex( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ), DrawAttribs.NumInstances, DrawAttribs.BaseVertex );
                    }
                    else
                    {
                        if( DrawAttribs.FirstInstanceLocation )
                            glDrawElementsInstancedBaseInstance( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ), DrawAttribs.NumInstances, DrawAttribs.FirstInstanceLocation );
                        else
                            glDrawElementsInstanced( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ), DrawAttribs.NumInstances );
                    }
                }
                else
                {
                    if( DrawAttribs.FirstInstanceLocation )
                        glDrawArraysInstancedBaseInstance( GlTopology, DrawAttribs.StartVertexLocation, DrawAttribs.NumVertices, DrawAttribs.NumInstances, DrawAttribs.FirstInstanceLocation );
                    else
                        glDrawArraysInstanced( GlTopology, DrawAttribs.StartVertexLocation, DrawAttribs.NumVertices, DrawAttribs.NumInstances );
                }
            }
            else
            {
                if( DrawAttribs.IsIndexed )
                {
                    if( DrawAttribs.BaseVertex )
                        glDrawElementsBaseVertex( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ), DrawAttribs.BaseVertex );
                    else
                        glDrawElements( GlTopology, DrawAttribs.NumIndices, IndexType, reinterpret_cast<GLvoid*>( static_cast<size_t>(FirstIndexByteOffset) ) );
                }
                else
                    glDrawArrays( GlTopology, DrawAttribs.StartVertexLocation, DrawAttribs.NumVertices );
            }
            CHECK_GL_ERROR( "OpenGL draw command failed" );
        }

        // IMPORTANT: new pending memory barriers in the context must be set
        // after all previous barriers have been executed.
        // m_CommitedResourcesTentativeBarriers contains memory barriers that will be required 
        // AFTER the actual draw/dispatch command is executed. 
        m_ContextState.SetPendingMemoryBarriers( m_CommitedResourcesTentativeBarriers );
        m_CommitedResourcesTentativeBarriers = 0;
    }

    void DeviceContextGLImpl::DispatchCompute( const DispatchComputeAttribs &DispatchAttrs )
    {
#if GL_ARB_compute_shader
        if( DispatchAttrs.pIndirectDispatchAttribs )
        {
            CHECK_DYNAMIC_TYPE( BufferGLImpl, DispatchAttrs.pIndirectDispatchAttribs );
            auto *pBufferOGL = static_cast<BufferGLImpl*>(DispatchAttrs.pIndirectDispatchAttribs);
            pBufferOGL->BufferMemoryBarrier(
                GL_COMMAND_BARRIER_BIT,// Command data sourced from buffer objects by
                                       // Draw*Indirect and DispatchComputeIndirect commands after the barrier
                                       // will reflect data written by shaders prior to the barrier.The buffer 
                                       // objects affected by this bit are derived from the DRAW_INDIRECT_BUFFER 
                                       // and DISPATCH_INDIRECT_BUFFER bindings.
                m_ContextState);

            glBindBuffer( GL_DISPATCH_INDIRECT_BUFFER, pBufferOGL->m_GlBuffer );
            CHECK_GL_ERROR( "Failed to bind a buffer for dispatch indirect command" );

            glDispatchComputeIndirect( DispatchAttrs.DispatchArgsByteOffset );
            CHECK_GL_ERROR( "glDispatchComputeIndirect() failed" );

            glBindBuffer( GL_DISPATCH_INDIRECT_BUFFER, 0 );
        }
        else
        {
            glDispatchCompute( DispatchAttrs.ThreadGroupCountX, DispatchAttrs.ThreadGroupCountY, DispatchAttrs.ThreadGroupCountZ );
            CHECK_GL_ERROR( "glDispatchCompute() failed" );
        }

        // IMPORTANT: new pending memory barriers in the context must be set
        // after all previous barriers have been executed.
        // m_CommitedResourcesTentativeBarriers contains memory barriers that will be required 
        // AFTER the actual draw/dispatch command is executed. 
        m_ContextState.SetPendingMemoryBarriers( m_CommitedResourcesTentativeBarriers );
        m_CommitedResourcesTentativeBarriers = 0;
#else
        UNSUPPORTED("Compute shaders are not supported");
#endif
    }

    void DeviceContextGLImpl::ClearDepthStencil( ITextureView *pView, Uint32 ClearFlags, float fDepth, Uint8 Stencil )
    {
        // Unlike OpenGL, in D3D10+, the full extent of the resource view is always cleared. 
        // Viewport and scissor settings are not applied.
        if( pView != nullptr )
        {
            const auto& ViewDesc = pView->GetDesc();
            VERIFY( ViewDesc.ViewType == TEXTURE_VIEW_DEPTH_STENCIL, "Incorrect view type: depth stencil is expected" );
            CHECK_DYNAMIC_TYPE( TextureViewGLImpl, pView );
            if( pView != m_pBoundDepthStencil )
            {
                UNEXPECTED( "Depth stencil buffer being cleared is not bound to the pipeline" );
                LOG_ERROR_MESSAGE( "Depth stencil buffer must be bound to the pipeline to be cleared" );
            }
        }
        else
        {
            if( !m_IsDefaultFramebufferBound )
            {
                UNEXPECTED( "Default depth stencil buffer being cleared is not bound to the pipeline" );
                LOG_ERROR_MESSAGE( "Default depth stencil buffer must be bound to the pipeline to be cleared" );
            }
        }
        Uint32 glClearFlags = 0;
        if( ClearFlags & CLEAR_DEPTH_FLAG )   glClearFlags |= GL_DEPTH_BUFFER_BIT;
        if( ClearFlags & CLEAR_STENCIL_FLAG ) glClearFlags |= GL_STENCIL_BUFFER_BIT;
        glClearDepthf( fDepth );
        glClearStencil( Stencil );
        // If depth writes are disabled, glClear() will not clear depth buffer!
        bool DepthWritesEnabled = m_ContextState.GetDepthWritesEnabled();
        m_ContextState.EnableDepthWrites( True );
        bool ScissorTestEnabled = m_ContextState.GetScissorTestEnabled();
        m_ContextState.EnableScissorTest( False );
        // The pixel ownership test, the scissor test, dithering, and the buffer writemasks affect 
        // the operation of glClear. The scissor box bounds the cleared region. Alpha function, 
        // blend function, logical operation, stenciling, texture mapping, and depth-buffering 
        // are ignored by glClear.
        glClear(glClearFlags);
        CHECK_GL_ERROR( "glClear() failed" );
        m_ContextState.EnableDepthWrites( DepthWritesEnabled );
        m_ContextState.EnableScissorTest( ScissorTestEnabled );
    }

    void DeviceContextGLImpl::ClearRenderTarget( ITextureView *pView, const float *RGBA )
    {
        // Unlike OpenGL, in D3D10+, the full extent of the resource view is always cleared. 
        // Viewport and scissor settings are not applied.

        Int32 RTIndex = -1;
        if( pView != nullptr )
        {
            const auto& ViewDesc = pView->GetDesc();
            VERIFY( ViewDesc.ViewType == TEXTURE_VIEW_RENDER_TARGET, "Incorrect view type: render target is expected" );
            CHECK_DYNAMIC_TYPE( TextureViewGLImpl, pView );
            for( Uint32 rt = 0; rt < m_NumBoundRenderTargets; ++rt )
                if( m_pBoundRenderTargets[rt] == pView )
                {
                    RTIndex = rt;
                    break;
                }

            if( RTIndex == -1 )
            {
                UNEXPECTED( "Render target being cleared is not bound to the pipeline" );
                LOG_ERROR_MESSAGE( "Render target must be bound to the pipeline to be cleared" );
            }
        }
        else
        {
            if( m_IsDefaultFramebufferBound )
                RTIndex = 0;
            else
            {
                UNEXPECTED( "Default render target must be bound to the pipeline to be cleared" );
                LOG_ERROR_MESSAGE( "Default render target must be bound to the pipeline to be cleared" );
            }
        }

        static const float Zero[4] = { 0, 0, 0, 0 };
        if( RGBA == nullptr )
            RGBA = Zero;
        
        // The pixel ownership test, the scissor test, dithering, and the buffer writemasks affect 
        // the operation of glClear. The scissor box bounds the cleared region. Alpha function, 
        // blend function, logical operation, stenciling, texture mapping, and depth-buffering 
        // are ignored by glClear.

        // Disable scissor test
        bool ScissorTestEnabled = m_ContextState.GetScissorTestEnabled();
        m_ContextState.EnableScissorTest( False );

        // Set write mask
        Uint32 WriteMask = 0;
        Bool bIndependentBlend = False;
        m_ContextState.GetColorWriteMask( RTIndex, WriteMask, bIndependentBlend );
        m_ContextState.SetColorWriteMask( RTIndex, COLOR_MASK_ALL, bIndependentBlend );
        
        glClearBufferfv( GL_COLOR, RTIndex, RGBA );
        CHECK_GL_ERROR( "glClearBufferfv() failed" );

        m_ContextState.SetColorWriteMask( RTIndex, WriteMask, bIndependentBlend );
        m_ContextState.EnableScissorTest( ScissorTestEnabled );
    }

    void DeviceContextGLImpl::Flush()
    {
        glFlush();
    }

    void DeviceContextGLImpl::FinishCommandList(class ICommandList **ppCommandList)
    {
        LOG_ERROR("Deferred contexts are not supported in OpenGL mode");
    }

    void DeviceContextGLImpl::ExecuteCommandList(class ICommandList *pCommandList)
    {
        LOG_ERROR("Deferred contexts are not supported in OpenGL mode");
    }

    bool DeviceContextGLImpl::UpdateCurrentGLContext()
    {
        auto *pRenderDeviceGL = ValidatedCast<RenderDeviceGLImpl>(m_pDevice.RawPtr());
        auto NativeGLContext = pRenderDeviceGL->m_GLContext.GetCurrentNativeGLContext();
        if (NativeGLContext == NULL)
            return false;

        m_ContextState.SetCurrentGLContext(NativeGLContext);
        return true;
    }
}
