"""Particle draw-bin arithmetic/source contracts; native GPU validation is separate."""
from pathlib import Path
import random
import re
import struct
import unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'Content/Shaders/project_hs.hlsl').read_text(encoding='utf-8-sig')

def metadata(renderer,primitive=0,selector=0):
    return (renderer<<8)|(primitive<<16)|(selector<<24)

def expected_bin(value):
    renderer=(value>>8)&255
    if renderer!=3:return 0
    primitive=(value>>16)&255;selector=value>>24
    if selector:return selector
    return {7:1,10:7,11:8}.get(primitive,2)

class ParticleBins(unittest.TestCase):
    def test_exact_mesh_counts_keep_all_non_degenerate_triangles(self):
        rng=random.Random(41807)
        for _ in range(80):
            counts=[3*rng.randrange(8,500) for _ in range(7)]
            maximum=max(6,*counts)
            values=[metadata(mode) for mode in range(3)]
            values += [metadata(3,primitive) for primitive in range(16)]
            values += [metadata(3,0,selector) for selector in range(1,8)]
            for value in values:
                selected=expected_bin(value)
                actual=6 if selected==0 else 24 if selected==8 else counts[selected-1]
                old=6 if selected==0 else maximum
                old_vertices=[i if i<actual else 0 for i in range(old)]
                meaningful=[tuple(old_vertices[i:i+3]) for i in range(0,old,3) if len(set(old_vertices[i:i+3]))>1]
                exact=[(i,i+1,i+2) for i in range(0,actual,3)]
                self.assertEqual(meaningful,exact)
                self.assertLessEqual(actual,old)

    def test_capacity_bins_are_disjoint_and_indirect_layout_is_twenty_bytes(self):
        capacity=10000
        for selected in range(9):
            first=capacity*(1+selected);last=first+capacity-1
            self.assertLess(last,10*capacity)
            if selected:self.assertEqual(first-capacity,capacity*selected)
            command=struct.pack('<5I',selected,24,capacity,0,0)
            self.assertEqual(len(command),20)
            self.assertEqual(struct.unpack('<5I',command),(selected,24,capacity,0,0))
        self.assertEqual((3+9)*4,48)

    def test_capture_counts_all_bins_after_root_constant(self):
        instances=[0,1,2,3,0,4,0,5,6]
        packet=b''.join(struct.pack('<5I',bin,6 if bin==0 else 24,count,0,0) for bin,count in enumerate(instances))
        self.assertEqual(sum(struct.unpack_from('<5I',packet,bin*20)[2] for bin in range(9)),sum(instances))
        self.assertEqual(struct.unpack_from('<5I',packet,0)[1],6)  # Old decoder misread vertex count as alive.
        capture=(ROOT/'Source/RendererD3D12/Private/renderer_capture.cpp').read_text(encoding='utf-8-sig')
        self.assertIn('total_size + particle_argument_bytes',capture)
        self.assertIn('bin * kRibbonArgumentStride + sizeof(std::uint32_t)',capture)
        self.assertIn('last_particle_count += particle_draws[bin].InstanceCount',capture)

    def test_shader_and_existing_signature_contract(self):
        classify=SOURCE.split('uint ParticleDrawBin(',1)[1].split('[shader("compute")]',1)[0]
        for text in ('return 0u','return selector','primitive == 11u ? 8u','primitive == 7u ? 1u','primitive == 10u ? 7u : 2u'):
            self.assertIn(text,classify)
        finalize=SOURCE.split('if (PassValue == 5 && index == 0)',1)[1].split('struct ParticleOutput',1)[0]
        self.assertIn('bin < 9u',finalize)
        self.assertIn('bin * 20u',finalize)
        self.assertIn('IndirectArguments.Store(argument, bin)',finalize)
        self.assertNotIn('max(vertex_count',finalize)
        self.assertIn('VfxMeshAtlas.Load(24u + (bin - 1u) * 12u)',finalize)
        vertex=SOURCE.split('ParticleOutput ParticleVS(',1)[1].split('ParticleOutput GroundRingVS(',1)[0]
        self.assertIn('ParticleOptions.x * (1 + PassValue) + instance_id',vertex)
        impl=(ROOT/'Source/RendererD3D12/Private/renderer_impl.hpp').read_text(encoding='utf-8-sig')
        self.assertRegex(impl,r'kParticleDrawBinCount\s*=\s*9')
        resources=(ROOT/'Source/RendererD3D12/Private/renderer_resources.cpp').read_text(encoding='utf-8-sig')
        for text in ('(1 + kParticleDrawBinCount) * kParticleCount','(3 + kParticleDrawBinCount) * sizeof(std::uint32_t)','kParticleDrawBinCount * kRibbonArgumentStride'):
            self.assertIn(text,resources)
        frame=(ROOT/'Source/RendererD3D12/Private/renderer_frame.cpp').read_text(encoding='utf-8-sig')
        self.assertIn('ExecuteIndirect(impl_->ribbon_draw_signature.Get(), kParticleDrawBinCount',frame)
        signature=(ROOT/'Source/RendererD3D12/Private/renderer_pipeline.cpp').read_text(encoding='utf-8-sig').split('std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> ribbon_arguments',1)[1].split('return CreateClusterPipelines',1)[0]
        for text in ('Constant.RootParameterIndex = 6','D3D12_INDIRECT_ARGUMENT_TYPE_DRAW','signature.ByteStride = kRibbonArgumentStride'):
            self.assertIn(text,signature)

if __name__=='__main__':unittest.main()
