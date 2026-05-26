# WorkboardManager — GUI dashboard with embedded LLM inference

set (TARGET_NAME WorkboardManager)

# Source files
define_source_files ()

setup_main_executable (NOBUNDLE)

# llama.cpp headers (targets already built by YukiHoho's CMakeLists)
target_include_directories (${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/llama.cpp/include)

# Static link group — llama pulls Vulkan symbols that drag in all of libUrho3D.a,
# exposing circular deps with Bullet/IK/Box2D. Group lets the linker resolve them.
set (TP ${CMAKE_BINARY_DIR}/Source/ThirdParty)
target_link_libraries (${TARGET_NAME}
    -Wl,--start-group
    ${CMAKE_BINARY_DIR}/lib/libUrho3D.a
    llama ggml ggml-base ggml-cpu ggml-vulkan
    ${TP}/Bullet/libBullet.a
    ${TP}/Box2D/libBox2D.a
    ${TP}/ik/libik.a
    ${TP}/Recast/libRecast.a
    ${TP}/Detour/libDetour.a
    ${TP}/DetourCrowd/libDetourCrowd.a
    ${TP}/DetourTileCache/libDetourTileCache.a
    ${TP}/SLikeNet/libSLikeNet.a
    ${TP}/Civetweb/libCivetweb.a
    ${TP}/AngelScript/libAngelScript.a
    ${TP}/FreeType/libFreeType.a
    ${TP}/PugiXml/libPugiXml.a
    ${TP}/LZ4/libLZ4.a
    ${TP}/StanHull/libStanHull.a
    ${TP}/SDL/libSDL.a
    ${TP}/SQLite/libsqlite.a
    ${TP}/WebP/libWebP.a
    ${TP}/ETCPACK/libETCPACK.a
    ${TP}/VMA/libVulkanMemoryAllocator.a
    ${TP}/Assimp/code/libAssimp.a
    ssl crypto dl pthread m rt
    -Wl,--end-group)
