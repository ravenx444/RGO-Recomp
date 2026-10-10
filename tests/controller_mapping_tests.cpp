#include "jfg/runtime/controller_mapping.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    check(argc == 2, "fixture argument required");
    std::ifstream file(argv[1], std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    jfg::ControllerMapping mapping;
    check(jfg::parse_controller_mapping(text, mapping), "shared default profile rejected");
    jfg::StandardControllerSample input;
    input.buttons[0] = true; input.buttons[6] = true; input.axes[4] = 10000;
    input.axes[0] = 32767; input.axes[1] = -32768; input.axes[3] = -10000;
    auto output = jfg::map_controller(mapping, input);
    check(output.buttons == 0xB008 && output.stick == jfg::N64StickSample{80,80}, "default gameplay buttons and axes");
    mapping.bindings[3] = 3; input.buttons[6] = false;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) == 0, "old Start binding still active");
    input.buttons[3] = true;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) != 0, "new Start binding not active");
    mapping.bindings[3] = -1;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) == 0, "unbound Start remains active");
    // RGO: touchpad click (27) and Mic/Share/Capture (28) are bindable.
    mapping.bindings[3] = jfg::kBindingTouchpad; input.buttons[15] = true;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) != 0, "touchpad binding not active");
    input.buttons[15] = false; mapping.bindings[3] = jfg::kBindingMisc; input.buttons[16] = true;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) != 0, "misc binding not active");
    input.buttons[16] = false;
    check((jfg::map_controller(mapping,input).buttons & 0x1000) == 0, "released misc binding active");
    {
        auto touch = text; touch.replace(touch.find("map3="), 6, "map3=27");
        jfg::ControllerMapping parsed;
        check(jfg::parse_controller_mapping(touch, parsed) && parsed.bindings[3] == 27, "touchpad binding rejected");
    }
    mapping.bindings[3] = -1;
    mapping.stick = 1; mapping.invert_x = 1; mapping.invert_y = 1;
    input.axes[2] = 32767; input.axes[3] = -32768;
    check(jfg::map_controller(mapping,input).stick == jfg::N64StickSample{-80,-80}, "stick selection/inversion");
    mapping.deadzone = 20000; input.axes[2] = 15000; input.axes[3] = 0;
    check(jfg::map_controller(mapping,input).stick == jfg::N64StickSample{}, "custom dead zone");
    for (const auto& invalid : {text+"device=1\n", text+"unknown=1\n", std::string("version=1\n"),
                              std::string(4097,'x')}) {
        const auto before = mapping.bindings;
        check(!jfg::parse_controller_mapping(invalid,mapping), "malformed mapping accepted");
        check(mapping.bindings == before, "invalid profile changed active bindings");
    }
    for (const auto& replacement : {std::pair{"deadzone=7849","deadzone=32767"},
        std::pair{"map0=0","map0=29"}, std::pair{"device=-1","device=4"}, std::pair{"invert_x=0","invert_x=2"}}) {
        auto bad=text; bad.replace(bad.find(replacement.first),std::string(replacement.first).size(),replacement.second);
        check(!jfg::parse_controller_mapping(bad,mapping),"out-of-range profile accepted");
    }
    return 0;
}
