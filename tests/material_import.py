"""Owned PNG fixtures exercise our import pipeline, not encoder internals."""
import json, math, pathlib, struct, subprocess, sys, tempfile, zlib

def png(path, pixels, size):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    data = b''.join(b'\0' + bytes(pixels[y*size*4:(y+1)*size*4]) for y in range(size))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(data)) + chunk(b'IEND', b''))

with tempfile.TemporaryDirectory(prefix='ow-material-import-') as tmp:
    root = pathlib.Path(tmp)
    colour, height = [], []
    for y in range(8):
        for x in range(8):
            colour += [255 if (x+y)%2 else 0]*3 + [255]
            height += [round(127.5 + 64*math.sin(x*math.tau/8))]*3 + [255]
    png(root/'colour.png', colour, 8)
    png(root/'height.png', height, 8)
    recipe = dict(size=8, materials=[dict(name=n, colour='colour.png', height='height.png', repeatMetres=4, heightMetres=.1, roughness=.6) for n in ['grass','soil','rock','gravel']])
    (root/'recipe.json').write_text(json.dumps(recipe))
    subprocess.run([sys.argv[1],str(root/'recipe.json'),str(root/'out')],check=True)
    colour = (root/'out/colour_rgba.dds').read_bytes()[148:]
    normal = (root/'out/normal_rgba.dds').read_bytes()[148:]
    scalar = (root/'out/scalar_rgba.dds').read_bytes()[148:]
    assert abs(colour[256]-188)<=1, 'colour mips must average linear light'
    assert normal[0]<128 and abs(normal[1]-128)<=1 and normal[2]>250, 'wrap derivative and normal handedness'
    assert normal[4*4]>128, 'opposite height slope'
    assert scalar[0]==153 and scalar[1]==255 and scalar[3]==255, 'scalar convention'
    assert scalar[256]>=scalar[0], 'normal variance must not sharpen roughness'
    for name, fmt in [('colour',99),('normal',83),('scalar',98)]:
        data=(root/f'out/{name}.dds').read_bytes()
        assert struct.unpack_from('<I',data,128)[0]==fmt
        assert len(data)==148+4*(64+16+16+16), 'compressed mip/block sizes'
    # Constant colour must normalize to neutral RGB independently of source hue;
    # alpha is linear and separately authored, never inferred from this colour.
    png(root/'macro.png', [40,120,200,255]*64, 8)
    recipe['macroSize']=8
    for m in recipe['materials']:
        m.update(macroColour='macro.png', macroRoughnessAmplitude=0)
    (root/'recipe.json').write_text(json.dumps(recipe))
    subprocess.run([sys.argv[1],str(root/'recipe.json'),str(root/'out'),'--macro-only'],check=True)
    macro=(root/'out/macro_rgba.dds').read_bytes()[148:]
    assert all(abs(macro[i]-188)<=1 for i in range(len(macro)) if i%4!=3), 'neutral sRGB macro and mips'
    assert all(macro[i]==128 for i in range(3,len(macro),4)), 'linear roughness neutral'
    assert (root/'out/colour_rgba.dds').read_bytes()[148:]==colour, 'macro-only preserves micro assets'
    for m in recipe['materials']: m['macroRoughnessAmplitude']=.35
    (root/'recipe.json').write_text(json.dumps(recipe))
    subprocess.run([sys.argv[1],str(root/'recipe.json'),str(root/'out'),'--macro-only'],check=True)
    macro=(root/'out/macro_rgba.dds').read_bytes()[148:]
    assert max(macro[3:256:4])-min(macro[3:256:4])>30, 'independent roughness variation'
    assert all(abs(macro[i]-188)<=1 for i in range(len(macro)) if i%4!=3), 'roughness does not affect RGB'
    del recipe['materials'][0]['macroColour']
    (root/'recipe.json').write_text(json.dumps(recipe))
    assert subprocess.run([sys.argv[1],str(root/'recipe.json'),str(root/'bad'),'--macro-only'],capture_output=True).returncode!=0
    recipe['materials'][0]['heightMetres']=-1
    (root/'recipe.json').write_text(json.dumps(recipe))
    assert subprocess.run([sys.argv[1],str(root/'recipe.json'),str(root/'bad')],capture_output=True).returncode!=0
print('Material import invariants passed')
