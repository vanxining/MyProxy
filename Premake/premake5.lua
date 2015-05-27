-- premake5.lua

solution "MyProxy"
    configurations { "Debug", "Release" }

    project "MyProxy"
        kind "ConsoleApp"
        language "C++"
        flags "Unicode"

        files { "../*.h", "../*.hpp", "../*.cpp" }
        vpaths {
           ["Headers"] = { "../*.h", "../*.hpp", },
           ["Sources"] = "../*.cpp",
        }

        filter "configurations:Debug"
            defines { "_DEBUG", "DEBUG" }
            flags { "Symbols" }

        filter "configurations:Release"
            defines { "NDEBUG" }
            optimize "On"