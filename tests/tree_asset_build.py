"""Owned tiny PNG/recipe fixtures exercise incremental import and failed publication."""
import json, pathlib, struct, subprocess, sys, tempfile, time, zlib
cmake, importer, module, shaders = sys.argv[1:]
def png(path):
    def chunk(tag, data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    pixels=b''.join(b'\0'+b''.join(bytes((75,130,35,255 if 1<x<6 and 1<y<6 else 0)) for x in range(8)) for y in range(8))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b''))
with tempfile.TemporaryDirectory(prefix='tree-build-test-') as tmp:
    root=pathlib.Path(tmp)
    for name in ('beech','oak','birch','pine'):
        png(root/(name+'.png'));png(root/(name+'_bark.png'))
    recipe={'version':1,'frames':4,'tile_size':16,'texture_size':16,'attractors':32,'compressed':True,
            'trees':[{'species':0,'variant':0,'seed':17,'height':5,'radius':1.5}]}
    source=root/'recipe.json';source.write_text(json.dumps(recipe))
    (root/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(tree_fixture NONE)
add_executable(importer IMPORTED)
set_target_properties(importer PROPERTIES IMPORTED_LOCATION "{importer}")
include("{module}")
openworld_add_tree_assets(assets importer "${{CMAKE_CURRENT_SOURCE_DIR}}/recipe.json" "${{CMAKE_BINARY_DIR}}/out" "{shaders}")
''')
    def run(*args,ok=True):
        p=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        if (p.returncode==0)!=ok: raise AssertionError(p.stdout)
    run(cmake,'-S',str(root),'-B',str(root/'build'),'-G','Ninja')
    def build(ok=True):run(cmake,'--build',str(root/'build'),'--target','assets',ok=ok)
    build();out=root/'build/out';stamp=out/'.import-complete';library=out/'library.bin'
    before=stamp.stat().st_mtime_ns;payload=library.read_bytes()
    build();assert stamp.stat().st_mtime_ns==before,'unchanged build must be idle'
    time.sleep(.02);png(root/'beech.png');build();assert stamp.stat().st_mtime_ns>before
    assert library.read_bytes()==payload,'deterministic geometry across imports'
    (out/'tree0_normal_rgba.dds').unlink();build();assert (out/'tree0_normal_rgba.dds').exists()
    recipe['trees'][0]['height']=-1;source.write_text(json.dumps(recipe));build(ok=False)
    assert not stamp.exists(),'failed import must not publish completion'
    recipe['trees'][0]['height']=5;source.write_text(json.dumps(recipe));build();assert stamp.exists()
print('Tree incremental import checks passed')
