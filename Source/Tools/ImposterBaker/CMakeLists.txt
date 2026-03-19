# ImposterBaker — offline billboard texture generator for imposter trees
# Loads a .mdl, orbits camera, saves N transparent PNG snapshots

# Define target name
set (TARGET_NAME ImposterBaker)

# Define source files
define_source_files ()

# Setup target — needs graphics subsystem so use setup_main_executable
setup_main_executable (NOBUNDLE)
