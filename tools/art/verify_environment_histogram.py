"""Compare actual BC7 randomized blends against their source RGB histograms."""
import argparse, io, json, struct
from pathlib import Path
import numpy as np
from PIL import Image

def bc7_slice(path, layer, mip):
    raw=path.read_bytes();header=bytearray(raw[:148])
    width,height=struct.unpack_from('<II',raw,12)[::-1]
    mips=struct.unpack_from('<I',raw,28)[0]
    sizes=[max(1,(max(1,width>>m)+3)//4)*max(1,(max(1,height>>m)+3)//4)*16 for m in range(mips)]
    offset=148+layer*sum(sizes)+sum(sizes[:mip]);w=max(1,width>>mip);h=max(1,height>>mip)
    for at,value in [(12,h),(16,w),(20,sizes[mip]),(28,1),(140,1)]:struct.pack_into('<I',header,at,value)
    image=Image.open(io.BytesIO(header+raw[offset:offset+sizes[mip]])).convert('RGB')
    return np.asarray(image,dtype=np.float32).reshape(-1,3)/255

def linear(rgb):return np.where(rgb<=.04045,rgb/12.92,((rgb+.055)/1.055)**2.4)

def decode(g, table, origin, axes):
    t=np.clip(g*256-.5,0,255);a=t.astype(np.int32);b=np.minimum(a+1,255);f=t-a
    channels=np.arange(3)[None,:]
    p=table[a,channels]*(1-f)+table[b,channels]*f
    return np.clip(p@axes+origin,0,1)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('cooked',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    lut=np.frombuffer((args.cooked/'environment_terrain_histogram.dds').read_bytes(),dtype='<f4',offset=148).reshape(4,16,256,4)[...,:3]
    rng=np.random.default_rng(20260914);quantiles=np.linspace(.01,.99,99);records=[]
    for layer in range(4):
        source=linear(bc7_slice(args.cooked/'environment_terrain_basecolor.dds',layer,0))
        gaussian=bc7_slice(args.cooked/'environment_terrain_gaussian.dds',layer,0)
        origin=lut[layer,12,0];axes=lut[layer,13:16,0]
        positions=rng.integers(0,len(source),size=(3,65536));reference=source[rng.integers(0,len(source),size=65536)]
        reference_q=np.quantile(reference,quantiles,axis=0)
        for weights in [np.array([1/3,1/3,1/3]),np.array([.7,.2,.1])]:
            ordinary=(source[positions]*weights[:,None,None]).sum(axis=0)
            blended=.5+((gaussian[positions]*weights[:,None,None]).sum(axis=0)-.5)/np.sqrt(np.dot(weights,weights))
            restored=decode(blended,lut[layer,0],origin,axes)
            errors=[float(np.abs(np.quantile(values,quantiles,axis=0)-reference_q).mean()) for values in [ordinary,restored]]
            row={'layer':layer,'weights':weights.tolist(),'ordinary_rgb_quantile_mae':errors[0],'histogram_rgb_quantile_mae':errors[1],'error_reduction_percent':100*(1-errors[1]/errors[0]),'source_mean':reference.mean(axis=0).tolist(),'restored_mean':restored.mean(axis=0).tolist()}
            records.append(row)
            if not np.isfinite(restored).all() or errors[1]>=errors[0]:raise AssertionError('Histogram reconstruction failed to improve source distribution: '+str(row))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'method':'65536 independent randomized point-sample triplets per layer; actual cooked BC7 decoded; RGB marginal quantile comparison. Does not assert exact joint RGB or anisotropic-filtered preservation.','comparisons':records},indent=2),encoding='utf-8')
    print(json.dumps(records,indent=2))
if __name__=='__main__':main()
