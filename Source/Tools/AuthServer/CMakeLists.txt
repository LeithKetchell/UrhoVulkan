# AuthServer — private central authority binary
# NOT a public sample or tool — private server for TerrainNode network

# Define target name
set (TARGET_NAME AuthServer)

# Define source files
define_source_files ()

# Propagate database backend define (not auto-exported by Urho3D lib target)
if (URHO3D_DATABASE_SQLITE)
    add_definitions (-DURHO3D_DATABASE_SQLITE)
endif ()

# Setup target
setup_main_executable (NOBUNDLE)
