-- premake5.lua

solution "MyProxy"
    configurations { "Debug", "Release" }

    project "MyProxy"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++11"
        characterset "Unicode"

        files { "../*.h", "../*.hpp", "../*.cpp" }
        vpaths {
            ["Headers"] = { "../*.h", "../*.hpp", },
            ["Sources"] = "../*.cpp",
        }

        filter "configurations:Debug"
            defines { "_DEBUG", "DEBUG" }
            symbols "On"

        filter "configurations:Release"
            defines { "NDEBUG" }
            optimize "On"
