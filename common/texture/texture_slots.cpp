#include "texture_slots.h"

namespace {
std::vector<std::string> jak2_slots = {
    "jakbsmall-eyebrow",
    "jakbsmall-face",
    "jakbsmall-finger",
    "jakbsmall-hair",
    "jak-orig-arm-formorph",
    "jak-orig-eyebrow-formorph",
    "jak-orig-eyelid-formorph",
    "jak-orig-finger-formorph",
    "jakb-facelft",
    "jakb-facert",
    "jakb-hairtrans",
    "jakb-eyelid",
    "jakb-finger",
    "jakb-eyebrow",
    //"kor-eyeeffect-formorph",
    //"kor-hair-formorph",
    //"kor-head-formorph",
    //"kor-head-formorph-noreflect",
    //"kor-lowercaps-formorph",
    //"kor-uppercaps-formorph",
};

}

const std::vector<std::string>& jak2_animated_texture_slots() {
  return jak2_slots;
}
