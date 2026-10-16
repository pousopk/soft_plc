// soft_plc_monitor: a GUI that connects to a running soft_plc (started with
// --monitor) to watch, write and force its variables, like going online with
// a PLC in TIA Portal.
//
//   soft_plc_monitor                  connects to 127.0.0.1:5020
//   soft_plc_monitor <host> [port]
//   soft_plc_monitor --plot fast.out --plot slow.out    start with these in the trend
#include "monitor_window.hpp"
#include "poller.hpp"
#include <GLFW/glfw3.h>
#include <charconv>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5020;
    std::vector<std::string> plots;
    std::vector<std::string_view> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--plot" && i + 1 < argc) {
            plots.emplace_back(argv[++i]);
        } else {
            positional.push_back(arg);
        }
    }
    const auto usage = [] {
        std::fprintf(stderr, "usage: soft_plc_monitor [--plot VARIABLE]... [host] [port]\n");
        return 2;
    };
    if (positional.size() > 2) {
        return usage();
    }
    if (!positional.empty()) {
        host = std::string(positional[0]);
    }
    if (positional.size() == 2) {
        const std::string_view text = positional[1];
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0 || value > 65535) {
            return usage();
        }
        port = static_cast<std::uint16_t>(value);
    }

    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
    });
    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1200, 800, "soft_plc monitor", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync: draw at the screen's refresh rate, not as fast as possible

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // don't write imgui.ini next to the binary
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    {
        softplc::monitor::Poller poller;
        poller.connect(host, port);
        softplc::monitor::MonitorWindow monitorWindow(poller, host, port);
        for (const auto& path : plots) {
            monitorWindow.plot(path);
        }

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            monitorWindow.draw();

            ImGui::Render();
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.08F, 0.08F, 0.09F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    } // the poller's thread stops here, before the window goes away

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
