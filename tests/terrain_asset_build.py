"""Exercise the production CMake import rule with tiny, independently owned assets."""
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

cmake, importer, module = map(pathlib.Path, sys.argv[1:])


def png(path, value):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data)))
    pixels = (b'\0' + bytes([value, value, value, 255]) * 8) * 8
    path.write_bytes(b'\x89PNG\r\n\x1a\n'
                     + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def run(args, success=True):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True)
    if (result.returncode == 0) != success:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout + result.stderr


# Use a real newer timestamp, without future-dating files and creating clock skew.
def next_tick():
    time.sleep(1.05)


generators = [name for name, tool in [('Ninja', 'ninja'), ('Unix Makefiles', 'make')]
             if shutil.which(tool)]
assert generators, 'Need a supported local build tool to exercise asset dependencies'
for generator in generators:
    with tempfile.TemporaryDirectory(prefix='ow-asset-build-') as tmp:
        root = pathlib.Path(tmp)
        source, build = root/'source assets', root/'build assets'
        source.mkdir()
        local_importer = root/('importer' + importer.suffix)
        shutil.copy2(importer, local_importer)
        for name, value in [('colour.png', 80), ('height.png', 128), ('macro.png', 100),
                            ('other.png', 160)]:
            png(source/name, value)
        recipe = dict(size=8, macroSize=8, materials=[
            dict(name=name, colour='colour.png', height='height.png', macroColour='macro.png',
                 repeatMetres=4, heightMetres=.02, roughness=.8)
            for name in ['grass', 'soil', 'rock', 'gravel']])
        recipe_path = source/'palette.json'
        recipe_path.write_text(json.dumps(recipe))
        (source/'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.24)
project(AssetBuildFixture NONE)
add_executable(importer IMPORTED GLOBAL)
set_target_properties(importer PROPERTIES IMPORTED_LOCATION "{local_importer.as_posix()}")
include("{module.as_posix()}")
openworld_add_terrain_assets(terrain_assets importer
    "${{CMAKE_CURRENT_SOURCE_DIR}}/palette.json" "${{CMAKE_CURRENT_BINARY_DIR}}/runtime")
add_custom_target(fixture_engine)
add_dependencies(fixture_engine terrain_assets)
''')
        run([cmake, '-S', source, '-B', build, '-G', generator])
        output = build/'runtime'
        files = ['palette.json', '.import-complete'] + [f'{kind}{suffix}.dds'
                 for kind in ['colour', 'normal', 'scalar', 'macro'] for suffix in ['', '_rgba']]

        def snapshot():
            return {name: (output/name).stat().st_mtime_ns for name in files}

        def build_assets(success=True):
            return run([cmake, '--build', build, '--target', 'fixture_engine'], success)

        build_assets()
        before = snapshot()
        assert json.loads((output/'palette.json').read_text()) == recipe
        assert (output/'colour_rgba.dds').read_bytes()[148] == 80
        build_assets()
        assert snapshot() == before, 'Unchanged builds must not rerun the importer'

        next_tick()
        png(source/'colour.png', 120)
        build_assets()
        assert snapshot()['.import-complete'] > before['.import-complete']
        assert (output/'colour_rgba.dds').read_bytes()[148] == 120, 'Changed PNG must be imported'

        next_tick()
        for material in recipe['materials']:
            material['colour'] = 'other.png'
        recipe_path.write_text(json.dumps(recipe))
        build_assets()
        assert (output/'colour_rgba.dds').read_bytes()[148] == 160, 'Recipe edits must reconfigure dependencies'
        next_tick()
        png(source/'other.png', 200)
        build_assets()
        assert (output/'colour_rgba.dds').read_bytes()[148] == 200, 'New recipe references must be tracked'
        before = snapshot()
        next_tick()
        png(source/'colour.png', 240)
        build_assets()
        assert snapshot() == before, 'Removed recipe dependencies must not cause imports'

        for name in ['normal_rgba.dds', 'macro.dds', 'palette.json', '.import-complete']:
            (output/name).unlink()
            build_assets()
            assert all((output/item).is_file() for item in files), f'Missing {name} must be regenerated'

        before = snapshot()
        next_tick()
        os.utime(local_importer, None)
        build_assets()
        assert snapshot()['.import-complete'] > before['.import-complete'], 'Importer change must regenerate assets'

        next_tick()
        (source/'other.png').write_bytes(b'not a PNG')
        build_assets(success=False)
        assert not (output/'.import-complete').exists(), 'Failed import must not leave a success stamp'
        png(source/'other.png', 64)
        build_assets()
        assert (output/'colour_rgba.dds').read_bytes()[148] == 64, 'Failed imports must recover on the next build'

        next_tick()
        for material in recipe['materials']:
            del material['macroColour']
        recipe_path.write_text(json.dumps(recipe))
        build_assets()
        assert not (output/'macro.dds').exists() and not (output/'macro_rgba.dds').exists(), 'Removing optional macros must remove stale outputs'
        print(f'{generator}: asset build dependencies, missing outputs, failure/retry passed')
