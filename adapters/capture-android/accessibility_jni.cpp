// JNI boundary of the Android accessibility adapter (M5-05, compile-verified
// by the CI ndk job). The Java side walks AccessibilityNodeInfo trees (an
// AccessibilityService is platform plumbing Mirador never provides) and
// pushes one node per call; the C++ side only converts boundary values into
// `AndroidNodeRegion`s (RULE-02: JNI types stay in this file).
//
// Encoding caveat (documented, example scope): JNI GetStringUTFChars yields
// modified UTF-8, where supplementary characters appear as surrogate pairs.
// For full Unicode fidelity the Java side should pass
// `text.getBytes(StandardCharsets.UTF_8)` instead; this bridge demonstrates
// the plain-string path.
#include "accessibility_regions.hpp"

#include <jni.h>

#include <cstdint>
#include <new>
#include <string>
#include <vector>

namespace {

std::string copy_jstring(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string copied(chars);
    env->ReleaseStringUTFChars(value, chars);
    return copied;
}

/// JNI calls never propagate C++ exceptions; allocation failure surfaces as
/// a Java OutOfMemoryError instead of terminating the process.
void push_region(JNIEnv* env, std::vector<mirador::adapters::AndroidNodeRegion>& sink,
                 mirador::adapters::AndroidNodeRegion node) {
    try {
        sink.push_back(std::move(node));
    } catch (const std::bad_alloc&) {
        const jclass oom = env->FindClass("java/lang/OutOfMemoryError");
        if (oom != nullptr) {
            env->ThrowNew(oom, "accessibility adapter: region allocation failed");
        }
    }
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_dev_mirador_adapters_AccessibilityBridge_pushRegion(JNIEnv* env, jclass /*clazz*/, jintArray bounds,
                                                         jstring text, jstring role, jstring description,
                                                         jboolean interactive, jboolean enabled, jlong sink_ptr) {
    auto* sink = reinterpret_cast<std::vector<mirador::adapters::AndroidNodeRegion>*>(
        static_cast<uintptr_t>(sink_ptr));  // NOLINT(performance-no-int-to-ptr): JNI long-lived bridge idiom
    if (sink == nullptr || bounds == nullptr) {
        return;
    }
    jint edges[4] = {0, 0, 0, 0};
    env->GetIntArrayRegion(bounds, 0, 4, edges);

    mirador::adapters::AndroidNodeRegion node;
    node.left = edges[0];
    node.top = edges[1];
    node.right = edges[2];
    node.bottom = edges[3];
    node.text = copy_jstring(env, text);
    node.role = copy_jstring(env, role);
    node.description = copy_jstring(env, description);
    node.interactive = interactive == JNI_TRUE;
    node.enabled = enabled == JNI_TRUE;
    push_region(env, *sink, std::move(node));
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_mirador_adapters_AccessibilityBridge_createSink(JNIEnv* /*env*/, jclass /*clazz*/) {
    // Ownership transfers to the Java side, which must eventually call
    // destroySink with the same value (documented in the README).
    const auto* sink = new std::vector<mirador::adapters::AndroidNodeRegion>();  // NOLINT
    return reinterpret_cast<jlong>(sink);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_mirador_adapters_AccessibilityBridge_destroySink(JNIEnv* /*env*/, jclass /*clazz*/, jlong sink_ptr) {
    delete reinterpret_cast<std::vector<mirador::adapters::AndroidNodeRegion>*>(  // NOLINT
        static_cast<uintptr_t>(sink_ptr));
}
