"""Group range reservation: partition conservation, capacity and draw-bin agreement."""
import random
import unittest
from pathlib import Path

class ParticleGroup(unittest.TestCase):
    def test_compaction_and_bins_share_each_live_slot_once(self):
        rng=random.Random(9007)
        for capacity in (0,1,16,255,256,257,10000):
            source=[(i,rng.randrange(10)) for i in range(capacity)]
            groups=[source[i:i+256] for i in range(0,capacity,256)]
            rng.shuffle(groups) # Any legal group scheduling order.
            alive=[];dead=[];bins=[[] for _ in range(9)]
            for group in groups:
                categories=[[i for i,b in group if b==k] for k in range(10)]
                alive.extend(i for k in range(9) for i in categories[k])
                dead.extend(categories[9])
                for k in range(9):bins[k].extend(categories[k])
            self.assertEqual(sorted(alive+dead),list(range(capacity)))
            self.assertEqual(sorted(alive),sorted(i for b in bins for i in b))
            self.assertEqual(len(alive),len(set(alive)))
            for k,b in enumerate(bins):self.assertTrue(all(source[i][1]==k for i in b))
        shader=(Path(__file__).resolve().parents[1]/'Content/Shaders/project_hs.hlsl').read_text(encoding='utf-8-sig')
        helper=shader.split('void AppendParticleGroup(',1)[1].split('bool SpawnParticle(',1)[0]
        self.assertEqual(helper.count('GroupMemoryBarrierWithGroupSync()'),3)
        self.assertIn('ParticleGroupAliveBase + prefix + rank',helper)
        phase=shader.split('void ParticleCS(',1)[1].split('struct ParticleOutput',1)[0]
        self.assertNotIn('PassValue == 4',phase)
        self.assertNotIn('WritableParticles[particle_index] = particle',phase)
        self.assertIn('WritableParticles[particle_index].PositionLife = particle.PositionLife',phase)
        self.assertIn('AppendParticleGroup(lane, active, alive',phase)
        self.assertIn('AppendParticleGroup(lane, spawned, true',phase)
        self.assertIn('InterlockedCompareExchange',shader.split('bool SpawnParticle(',1)[1].split('void ParticleCS(',1)[0])

if __name__=='__main__':unittest.main()
