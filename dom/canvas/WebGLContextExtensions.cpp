/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebGLContext.h"
#include "WebGLContextUtils.h"
#include "WebGLExtensions.h"
#include "gfxPrefs.h"
#include "GLContext.h"

#include "nsString.h"
#include "mozilla/Preferences.h"
#include "AccessCheck.h"

namespace mozilla {

/*static*/ const char*
WebGLContext::GetExtensionString(WebGLExtensionID ext)
{
    typedef EnumeratedArray<WebGLExtensionID, WebGLExtensionID::Max,
                            const char*> names_array_t;

    static names_array_t sExtensionNamesEnumeratedArray;
    static bool initialized = false;

    if (!initialized) {
        initialized = true;

#define WEBGL_EXTENSION_IDENTIFIER(x) \
        sExtensionNamesEnumeratedArray[WebGLExtensionID::x] = #x;

        WEBGL_EXTENSION_IDENTIFIER(ANGLE_instanced_arrays)
        WEBGL_EXTENSION_IDENTIFIER(EXT_blend_minmax)
        WEBGL_EXTENSION_IDENTIFIER(EXT_color_buffer_half_float)
        WEBGL_EXTENSION_IDENTIFIER(EXT_frag_depth)
        WEBGL_EXTENSION_IDENTIFIER(EXT_shader_texture_lod)
        WEBGL_EXTENSION_IDENTIFIER(EXT_sRGB)
        WEBGL_EXTENSION_IDENTIFIER(EXT_texture_filter_anisotropic)
        WEBGL_EXTENSION_IDENTIFIER(EXT_disjoint_timer_query)
        WEBGL_EXTENSION_IDENTIFIER(OES_element_index_uint)
        WEBGL_EXTENSION_IDENTIFIER(OES_standard_derivatives)
        WEBGL_EXTENSION_IDENTIFIER(OES_texture_float)
        WEBGL_EXTENSION_IDENTIFIER(OES_texture_float_linear)
        WEBGL_EXTENSION_IDENTIFIER(OES_texture_half_float)
        WEBGL_EXTENSION_IDENTIFIER(OES_texture_half_float_linear)
        WEBGL_EXTENSION_IDENTIFIER(OES_vertex_array_object)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_color_buffer_float)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_compressed_texture_atc)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_compressed_texture_etc1)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_compressed_texture_pvrtc)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_compressed_texture_s3tc)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_debug_renderer_info)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_debug_shaders)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_depth_texture)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_draw_buffers)
        WEBGL_EXTENSION_IDENTIFIER(WEBGL_lose_context)

#undef WEBGL_EXTENSION_IDENTIFIER
    }

    return sExtensionNamesEnumeratedArray[ext];
}

bool
WebGLContext::IsExtensionEnabled(WebGLExtensionID ext) const
{
    return mExtensions[ext];
}

bool WebGLContext::IsExtensionSupported(JSContext* cx,
                                        WebGLExtensionID ext) const
{
    bool allowPrivilegedExts = false;

    // Chrome contexts need access to debug information even when
    // webgl.disable-extensions is set. This is used in the graphics
    // section of about:support
    if (NS_IsMainThread() &&
        xpc::AccessCheck::isChrome(js::GetContextCompartment(cx))) {
        allowPrivilegedExts = true;
    }

    if (gfxPrefs::WebGLPrivilegedExtensionsEnabled()) {
        allowPrivilegedExts = true;
    }

    if (allowPrivilegedExts) {
        switch (ext) {
        case WebGLExtensionID::WEBGL_debug_renderer_info:
            return true;
        case WebGLExtensionID::WEBGL_debug_shaders:
            return true;
        default:
            // For warnings-as-errors.
            break;
        }
    }

    return IsExtensionSupported(ext);
}

bool
WebGLContext::IsExtensionSupported(WebGLExtensionID ext) const
{
    if (mDisableExtensions)
        return false;

    // In alphabetical order
    switch (ext) {
    // ANGLE_
    case WebGLExtensionID::ANGLE_instanced_arrays:
        return WebGLExtensionInstancedArrays::IsSupported(this);

    // EXT_
    case WebGLExtensionID::EXT_blend_minmax:
        return WebGLExtensionBlendMinMax::IsSupported(this);
    case WebGLExtensionID::EXT_color_buffer_half_float:
        return WebGLExtensionColorBufferHalfFloat::IsSupported(this);
    case WebGLExtensionID::EXT_frag_depth:
        return WebGLExtensionFragDepth::IsSupported(this);
    case WebGLExtensionID::EXT_shader_texture_lod:
        return gl->IsExtensionSupported(gl::GLContext::EXT_shader_texture_lod);
    case WebGLExtensionID::EXT_sRGB:
        return WebGLExtensionSRGB::IsSupported(this);
    case WebGLExtensionID::EXT_texture_filter_anisotropic:
        return gl->IsExtensionSupported(gl::GLContext::EXT_texture_filter_anisotropic);

    // OES_
    case WebGLExtensionID::OES_element_index_uint:
        return gl->IsSupported(gl::GLFeature::element_index_uint);
    case WebGLExtensionID::OES_standard_derivatives:
        return gl->IsSupported(gl::GLFeature::standard_derivatives);
    case WebGLExtensionID::OES_texture_float:
        return gl->IsSupported(gl::GLFeature::texture_float);
    case WebGLExtensionID::OES_texture_float_linear:
        return gl->IsSupported(gl::GLFeature::texture_float_linear);
    case WebGLExtensionID::OES_texture_half_float:
        // If we have Feature::texture_half_float, we must not be on ES2
        // and need to translate HALF_FLOAT_OES -> HALF_FLOAT.  We do that
        // right before making the relevant calls.
        return gl->IsExtensionSupported(gl::GLContext::OES_texture_half_float) ||
               gl->IsSupported(gl::GLFeature::texture_half_float);

    case WebGLExtensionID::OES_texture_half_float_linear:
        return gl->IsSupported(gl::GLFeature::texture_half_float_linear);
    case WebGLExtensionID::OES_vertex_array_object:
        return true;

    // WEBGL_
    case WebGLExtensionID::WEBGL_color_buffer_float:
        return WebGLExtensionColorBufferFloat::IsSupported(this);
    case WebGLExtensionID::WEBGL_compressed_texture_atc:
        return gl->IsExtensionSupported(gl::GLContext::AMD_compressed_ATC_texture);
    case WebGLExtensionID::WEBGL_compressed_texture_etc1:
        return gl->IsExtensionSupported(gl::GLContext::OES_compressed_ETC1_RGB8_texture);
    case WebGLExtensionID::WEBGL_compressed_texture_pvrtc:
        return gl->IsExtensionSupported(gl::GLContext::IMG_texture_compression_pvrtc);
    case WebGLExtensionID::WEBGL_compressed_texture_s3tc:
        if (gl->IsExtensionSupported(gl::GLContext::EXT_texture_compression_s3tc))
            return true;

        return gl->IsExtensionSupported(gl::GLContext::EXT_texture_compression_dxt1) &&
               gl->IsExtensionSupported(gl::GLContext::ANGLE_texture_compression_dxt3) &&
               gl->IsExtensionSupported(gl::GLContext::ANGLE_texture_compression_dxt5);

    case WebGLExtensionID::WEBGL_debug_renderer_info:
        {
            bool isEnabled = true;

#ifdef RELEASE_BUILD
            // Keep this disabled on Release and Beta for now. (see bug 1171228)
            isEnabled = false;
#endif
            if (Preferences::GetBool("webgl.disable-debug-renderer-info", false))
                isEnabled = false;

            return isEnabled;
        }
    case WebGLExtensionID::WEBGL_depth_texture:
        // WEBGL_depth_texture supports DEPTH_STENCIL textures
        if (!gl->IsSupported(gl::GLFeature::packed_depth_stencil))
            return false;

        return gl->IsSupported(gl::GLFeature::depth_texture) ||
               gl->IsExtensionSupported(gl::GLContext::ANGLE_depth_texture);
    case WebGLExtensionID::WEBGL_draw_buffers:
        return WebGLExtensionDrawBuffers::IsSupported(this);
    case WebGLExtensionID::WEBGL_lose_context:
        // We always support this extension.
        return true;

    default:
        // For warnings-as-errors.
        break;
    }

    if (gfxPrefs::WebGLDraftExtensionsEnabled() || IsWebGL2()) {
        switch (ext) {
        case WebGLExtensionID::EXT_disjoint_timer_query:
            return WebGLExtensionDisjointTimerQuery::IsSupported(this);
        default:
            // For warnings-as-errors.
            break;
        }
    }

    return false;
}

static bool
CompareWebGLExtensionName(const nsACString& name, const char* other)
{
    return name.Equals(other, nsCaseInsensitiveCStringComparator());
}

WebGLExtensionBase*
WebGLContext::EnableSupportedExtension(JSContext* js, WebGLExtensionID ext)
{
    if (!IsExtensionEnabled(ext)) {
        if (!IsExtensionSupported(js, ext))
            return nullptr;

        EnableExtension(ext);
    }

    return mExtensions[ext];
}

void
WebGLContext::GetExtension(JSContext* cx, const nsAString& wideName,
                           JS::MutableHandle<JSObject*> retval, ErrorResult& rv)
{
    retval.set(nullptr);

    if (IsContextLost())
        return;

    NS_LossyConvertUTF16toASCII name(wideName);

    WebGLExtensionID ext = WebGLExtensionID::Unknown;

    // step 1: figure what extension is wanted
    for (size_t i = 0; i < size_t(WebGLExtensionID::Max); i++) {
        WebGLExtensionID extension = WebGLExtensionID(i);

        if (CompareWebGLExtensionName(name, GetExtensionString(extension))) {
            ext = extension;
            break;
        }
    }

    if (ext == WebGLExtensionID::Unknown) {
        // We keep backward compatibility for these deprecated vendor-prefixed
        // alias. Do not add new ones anymore. Hide it behind the
        // webgl.enable-draft-extensions flag instead.

        if (CompareWebGLExtensionName(name, "MOZ_WEBGL_lose_context")) {
            ext = WebGLExtensionID::WEBGL_lose_context;

        } else if (CompareWebGLExtensionName(name, "MOZ_WEBGL_compressed_texture_s3tc")) {
            ext = WebGLExtensionID::WEBGL_compressed_texture_s3tc;

        } else if (CompareWebGLExtensionName(name, "MOZ_WEBGL_compressed_texture_atc")) {
            ext = WebGLExtensionID::WEBGL_compressed_texture_atc;

        } else if (CompareWebGLExtensionName(name, "MOZ_WEBGL_compressed_texture_pvrtc")) {
            ext = WebGLExtensionID::WEBGL_compressed_texture_pvrtc;

        } else if (CompareWebGLExtensionName(name, "MOZ_WEBGL_depth_texture")) {
            ext = WebGLExtensionID::WEBGL_depth_texture;
        }

        if (ext != WebGLExtensionID::Unknown) {
            GenerateWarning("getExtension('%s'): MOZ_ prefixed WebGL extension"
                            " strings are deprecated. Support for them will be"
                            " removed in the future. Use unprefixed extension"
                            " strings. To get draft extensions, set the"
                            " webgl.enable-draft-extensions preference.",
                            name.get());
        }
    }

    if (ext == WebGLExtensionID::Unknown)
        return;

    // step 2: check if the extension is supported
    if (!IsExtensionSupported(cx, ext))
        return;

    // step 3: if the extension hadn't been previously been created, create it now, thus enabling it
    WebGLExtensionBase* extObj = EnableSupportedExtension(cx, ext);
    if (!extObj)
        return;

    // Step 4: Enable any implied extensions.
    switch (ext) {
    case WebGLExtensionID::OES_texture_float:
        EnableSupportedExtension(cx, WebGLExtensionID::WEBGL_color_buffer_float);
        break;

    case WebGLExtensionID::OES_texture_half_float:
        EnableSupportedExtension(cx, WebGLExtensionID::EXT_color_buffer_half_float);
        break;

    default:
        break;
    }

    retval.set(WebGLObjectAsJSObject(cx, extObj, rv));
}

void
WebGLContext::EnableExtension(WebGLExtensionID ext)
{
    MOZ_ASSERT(IsExtensionEnabled(ext) == false);

    WebGLExtensionBase* obj = nullptr;
    switch (ext) {
    // ANGLE_
    case WebGLExtensionID::ANGLE_instanced_arrays:
        obj = new WebGLExtensionInstancedArrays(this);
        break;

    // EXT_
    case WebGLExtensionID::EXT_blend_minmax:
        obj = new WebGLExtensionBlendMinMax(this);
        break;
    case WebGLExtensionID::EXT_color_buffer_half_float:
        obj = new WebGLExtensionColorBufferHalfFloat(this);
        break;
    case WebGLExtensionID::EXT_disjoint_timer_query:
        obj = new WebGLExtensionDisjointTimerQuery(this);
        break;
    case WebGLExtensionID::EXT_frag_depth:
        obj = new WebGLExtensionFragDepth(this);
        break;
    case WebGLExtensionID::EXT_shader_texture_lod:
        obj = new WebGLExtensionShaderTextureLod(this);
        break;
    case WebGLExtensionID::EXT_sRGB:
        obj = new WebGLExtensionSRGB(this);
        break;
    case WebGLExtensionID::EXT_texture_filter_anisotropic:
        obj = new WebGLExtensionTextureFilterAnisotropic(this);
        break;

    // OES_
    case WebGLExtensionID::OES_element_index_uint:
        obj = new WebGLExtensionElementIndexUint(this);
        break;
    case WebGLExtensionID::OES_standard_derivatives:
        obj = new WebGLExtensionStandardDerivatives(this);
        break;
    case WebGLExtensionID::OES_texture_float:
        obj = new WebGLExtensionTextureFloat(this);
        break;
    case WebGLExtensionID::OES_texture_float_linear:
        obj = new WebGLExtensionTextureFloatLinear(this);
        break;
    case WebGLExtensionID::OES_texture_half_float:
        obj = new WebGLExtensionTextureHalfFloat(this);
        break;
    case WebGLExtensionID::OES_texture_half_float_linear:
        obj = new WebGLExtensionTextureHalfFloatLinear(this);
        break;
    case WebGLExtensionID::OES_vertex_array_object:
        obj = new WebGLExtensionVertexArray(this);
        break;

    // WEBGL_
    case WebGLExtensionID::WEBGL_color_buffer_float:
        obj = new WebGLExtensionColorBufferFloat(this);
        break;
    case WebGLExtensionID::WEBGL_compressed_texture_atc:
        obj = new WebGLExtensionCompressedTextureATC(this);
        break;
    case WebGLExtensionID::WEBGL_compressed_texture_etc1:
        obj = new WebGLExtensionCompressedTextureETC1(this);
        break;
    case WebGLExtensionID::WEBGL_compressed_texture_pvrtc:
        obj = new WebGLExtensionCompressedTexturePVRTC(this);
        break;
    case WebGLExtensionID::WEBGL_compressed_texture_s3tc:
        obj = new WebGLExtensionCompressedTextureS3TC(this);
        break;
    case WebGLExtensionID::WEBGL_debug_renderer_info:
        obj = new WebGLExtensionDebugRendererInfo(this);
        break;
    case WebGLExtensionID::WEBGL_debug_shaders:
        obj = new WebGLExtensionDebugShaders(this);
        break;
    case WebGLExtensionID::WEBGL_depth_texture:
        obj = new WebGLExtensionDepthTexture(this);
        break;
    case WebGLExtensionID::WEBGL_draw_buffers:
        obj = new WebGLExtensionDrawBuffers(this);
        break;
    case WebGLExtensionID::WEBGL_lose_context:
        obj = new WebGLExtensionLoseContext(this);
        break;

    default:
        MOZ_ASSERT(false, "should not get there.");
    }

    mExtensions[ext] = obj;
}

void
WebGLContext::GetSupportedExtensions(JSContext* cx,
                                     dom::Nullable< nsTArray<nsString> >& retval)
{
    retval.SetNull();
    if (IsContextLost())
        return;

    nsTArray<nsString>& arr = retval.SetValue();

    for (size_t i = 0; i < size_t(WebGLExtensionID::Max); i++) {
        WebGLExtensionID extension = WebGLExtensionID(i);

        if (IsExtensionSupported(cx, extension)) {
            const char* extStr = GetExtensionString(extension);
            arr.AppendElement(NS_ConvertUTF8toUTF16(extStr));
        }
    }

    /**
     * We keep backward compatibility for these deprecated vendor-prefixed
     * alias. Do not add new ones anymore. Hide it behind the
     * webgl.enable-draft-extensions flag instead.
     */
    if (IsExtensionSupported(cx, WebGLExtensionID::WEBGL_lose_context))
        arr.AppendElement(NS_LITERAL_STRING("MOZ_WEBGL_lose_context"));
    if (IsExtensionSupported(cx, WebGLExtensionID::WEBGL_compressed_texture_s3tc))
        arr.AppendElement(NS_LITERAL_STRING("MOZ_WEBGL_compressed_texture_s3tc"));
    if (IsExtensionSupported(cx, WebGLExtensionID::WEBGL_compressed_texture_atc))
        arr.AppendElement(NS_LITERAL_STRING("MOZ_WEBGL_compressed_texture_atc"));
    if (IsExtensionSupported(cx, WebGLExtensionID::WEBGL_compressed_texture_pvrtc))
        arr.AppendElement(NS_LITERAL_STRING("MOZ_WEBGL_compressed_texture_pvrtc"));
    if (IsExtensionSupported(cx, WebGLExtensionID::WEBGL_depth_texture))
        arr.AppendElement(NS_LITERAL_STRING("MOZ_WEBGL_depth_texture"));
}

} // namespace mozilla
