"""Owned opacity fixtures exercise the real importer and CMake dependency rule."""
import json, pathlib, struct, subprocess, sys, tempfile, time, zlib

cmake, importer, module = (pathlib.Path(p).resolve() for p in sys.argv[1:4])
gpu = pathlib.Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None

def png(path, green=150):
    size=32
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    pixels=bytearray()
    for y in range(size):
        pixels.append(0)
        for x in range(size):
            covered=5<=x<27 and 3<=y<29 and (x+y)%7!=0
            # Transparent black must not bleed into valid leaf colour.
            pixels.extend((90,green,50,255) if covered else (0,0,0,0))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',size,size,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b''))

def run(args,success=True):
    result=subprocess.run(list(map(str,args)),capture_output=True,text=True)
    assert (result.returncode==0)==success,result.stdout+result.stderr

with tempfile.TemporaryDirectory(prefix='ow-foliage-build-') as tmp:
    root=pathlib.Path(tmp);source=root/'source assets';source.mkdir();build=root/'build assets';output=build/'runtime'
    png(source/'leaf.png')
    recipe={'version':1,'size':32,'grass':{'blades':16,'width':.018,'height':.65,'spread':.23},'leaves':[{'colour':'leaf.png','normalDome':.12}]*2}
    recipe_path=source/'recipe.json';recipe_path.write_text(json.dumps(recipe))
    (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(FoliageBuildFixture NONE)
add_executable(importer IMPORTED GLOBAL)
set_target_properties(importer PROPERTIES IMPORTED_LOCATION "{importer.as_posix()}")
include("{module.as_posix()}")
openworld_add_vegetation_assets(assets importer "${{CMAKE_CURRENT_SOURCE_DIR}}/recipe.json" "${{CMAKE_CURRENT_BINARY_DIR}}/runtime")
''')
    run([cmake,'-S',source,'-B',build,'-G','Ninja'])
    def build_assets(success=True):run([cmake,'--build',build,'--target','assets'],success)
    build_assets()
    stamp=output/'.import-complete';before=stamp.stat().st_mtime_ns
    build_assets();assert stamp.stat().st_mtime_ns==before,'no-op build imported again'
    data=(output/'texture0_rgba.dds').read_bytes()[148:]
    base=sum(a>=128 for a in data[3:32*32*4:4])/(32*32)
    offset=0
    for size in [32,16,8,4,2,1]:
        mip=data[offset:offset+size*size*4];offset+=len(mip)
        coverage=sum(a>=128 for a in mip[3::4])/(size*size)
        if size>=8:assert abs(coverage-base)<.08,(size,coverage,base)
        assert all(abs(mip[i]-[90,150,50][i%4])<=1 for i in range(len(mip)) if i%4!=3),'transparent RGB contamination'
    normal=(output/'texture1_rgba.dds').read_bytes()[148:]
    assert normal[0]<128 and normal[1]<128 and normal[2]>250,'normal orientation'
    assert normal[(31*32+31)*4]>128,'normal dome orientation'
    if gpu:run([gpu,output])
    time.sleep(1.05);png(source/'leaf.png',180);build_assets()
    assert stamp.stat().st_mtime_ns>before,'changed PNG not imported'
    assert (output/'texture0_rgba.dds').read_bytes()[149]==180
    (output/'texture1.dds').unlink();build_assets();assert (output/'texture1.dds').exists(),'missing output not rebuilt'
    time.sleep(1.05);recipe['grass']['blades']=24;recipe_path.write_text(json.dumps(recipe));build_assets()
    time.sleep(1.05);(source/'leaf.png').write_bytes(b'broken');build_assets(False);assert not stamp.exists(),'failed build left stamp'
    png(source/'leaf.png');build_assets();assert stamp.exists(),'failed import not recoverable'
print('Vegetation asset build/import checks passed')
