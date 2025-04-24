#include "triton_jit/jit_utils.h"

#include <dlfcn.h>  // dladdr
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include "pybind11/embed.h"

namespace triton_jit {

LibraryInit::LibraryInit() {
  c10::initLogging();
  ensure_python_initialized();
}

void ensure_python_initialized() {
  namespace py = pybind11;
  static bool initialized = false;
  if (!initialized && !Py_IsInitialized()) {
    static py::scoped_interpreter guard {};
    initialized = true;
  }
}

static LibraryInit library_init;

std::string execute_command(std::string_view command) {
  std::array<char, 128> buffer;
  std::string result;

  // Open the process and read its output
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.data(), "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("popen() failed to open pipe for command: " + std::string(command));
  }

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  while (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

const char *get_python_executable() {
  const static std::string python_executable_path = []() {
    std::string python_exe;
    const char *python_env = std::getenv("PYTHON_EXECUTABLE");
    python_exe = python_env ? std::string(python_env) : execute_command("which python");
    LOG(INFO) << "python executable: " << python_exe;
    if (python_exe.empty()) {
      throw std::runtime_error("cannot find python executable!");
    }
    return python_exe;
  }();
  return python_executable_path.c_str();
}

std::filesystem::path get_path_of_this_library() {
  // This function gives the library path of this library as runtime, similar to the $ORIGIN
  // that is used for run path (RPATH), but unfortunately, for custom dependencies (instead of linking)
  // there is no build system generator to take care of this.
  static const std::filesystem::path cached_path = []() {
    Dl_info dl_info;
    if (dladdr(reinterpret_cast<void *>(&get_path_of_this_library), &dl_info) && dl_info.dli_fname) {
      return std::filesystem::canonical(dl_info.dli_fname);  // Ensure absolute, resolved path
    } else {
      throw std::runtime_error("cannot get the path of libjit_utils.so");
    }
  }();
  return cached_path;
}

std::filesystem::path get_script_dir() {
  const static std::filesystem::path script_dir = []() {
    std::filesystem::path installed_script_dir =
        get_path_of_this_library().parent_path().parent_path() / "share" / "triton_jit" / "scripts";
    if (std::filesystem::exists(installed_script_dir)) {
      return installed_script_dir;
    } else {
      std::filesystem::path source_script_dir =
          std::filesystem::path(__FILE__).parent_path().parent_path() / "scripts";
      return source_script_dir;
    }
  }();
  return script_dir;
}

const char *get_gen_static_sig_script() {
  std::filesystem::path script_dir = get_script_dir();
  return (script_dir / "gen_ssig.py").c_str();
}

const char *get_standalone_compile_script() {
  std::filesystem::path script_dir = get_script_dir();
  return (script_dir / "standalone_compile.py").c_str();
}

std::filesystem::path get_home_directory() {
  const static std::filesystem::path home_dir = []() {
#ifdef _WIN32
    const char *home_dir_path = std::getenv("USERPROFILE");
#else
    const char *home_dir_path = std::getenv("HOME");
#endif
    return std::filesystem::path(home_dir_path);
  }();
  return home_dir;
}

std::filesystem::path get_cache_path() {
  const static std::filesystem::path cache_dir = []() {
    const char *cache_env = std::getenv("FLAGGEMS_TRITON_CACHE_DIR");
    std::filesystem::path cache_dir =
        cache_env ? std::filesystem::path(cache_env) : (get_home_directory() / ".flaggems" / "triton_cache");
    return cache_dir;
  }();
  return cache_dir;
}
}  // namespace triton_jit
