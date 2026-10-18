"""CPU collision/source contracts; native shader execution is a separate gate."""
import json
import math
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def mul(a, scale):
    return tuple(x * scale for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def velocity_after(v, drag, gravity, dt):
    return add(mul(v, math.exp(-drag * dt)), (0, gravity * dt, 0))


def advance(p, v, a, time):
    return add(add(p, mul(v, time)), mul(a, .5 * time * time))


def flat_step(p, v, dt, drag, gravity, restitution, floor):
    p = (p[0], max(p[1], floor), p[2])
    nv = velocity_after(v, drag, gravity, dt)
    np = add(p, mul(add(v, nv), .5 * dt))
    if np[1] < floor:
        acceleration = (nv[1] - v[1]) / dt
        height = p[1] - floor
        if height == 0:
            hit = -2 * v[1] / acceleration if v[1] > 0 and acceleration < 0 else 0
        elif acceleration == 0:
            hit = -height / v[1]
        else:
            hit = 2 * height / (-v[1] + math.sqrt(max(0, v[1] ** 2 - 2 * acceleration * height)))
        hit = min(max(hit, 0), dt)
        impact = add(v, mul(add(nv, mul(v, -1)), hit / dt))
        p = add(p, mul(add(v, impact), .5 * hit))
        p = (p[0], floor, p[2])
        v = (impact[0], max(0, -impact[1] * restitution), impact[2])
        nv = velocity_after(v, drag, gravity, dt - hit)
        np = add(p, mul(add(v, nv), .5 * (dt - hit)))
        if np[1] < floor:
            np, nv = (np[0], floor, np[2]), (nv[0], max(0, nv[1]), nv[2])
    return np, nv


def integrate(p, v, dt, height, *, drag=0, gravity=-9.81, restitution=.5,
              radius=0, plane=0, origin=(0, 0, 0), enabled=True):
    if not enabled:
        return flat_step(p, v, dt, drag, gravity, restitution, plane + radius)

    def floor(x, z):
        return plane + radius + height(x + origin[0], z + origin[2]) - origin[1]

    def normal(x, z):
        e = .002
        n = (floor(x-e, z) - floor(x+e, z), 2*e, floor(x, z-e) - floor(x, z+e))
        return mul(n, 1 / math.sqrt(dot(n, n)))

    nv = velocity_after(v, drag, gravity, dt)
    a = mul(add(nv, mul(v, -1)), 1 / dt)
    np = add(p, mul(add(v, nv), .5 * dt))
    minimum = min(p[1], np[1])
    if a[1] > 0:
        turn = min(max(-v[1] / a[1], 0), dt)
        minimum = min(minimum, advance(p, v, a, turn)[1])
    if minimum > plane + radius + .55 - origin[1]:
        return np, nv
    p = (p[0], max(p[1], floor(p[0], p[2])), p[2])
    np = add(p, mul(add(v, nv), .5 * dt))
    steps = min(64, max(1, math.ceil(max(math.sqrt(dot(v, v)), math.sqrt(dot(nv, nv))) * dt / .02)))
    lower, upper, hit = 0, dt, False
    for step in range(1, steps + 1):
        time = dt * step / steps
        probe = advance(p, v, a, time)
        if probe[1] < floor(probe[0], probe[2]):
            upper, hit = time, True
            break
        lower = time
    if hit:
        for _ in range(6):
            time = (lower + upper) * .5
            probe = advance(p, v, a, time)
            if probe[1] < floor(probe[0], probe[2]):
                upper = time
            else:
                lower = time
        p = advance(p, v, a, upper)
        p = (p[0], floor(p[0], p[2]), p[2])
        v = add(v, mul(add(nv, mul(v, -1)), upper / dt))
        n = normal(p[0], p[2])
        v = add(v, mul(n, -(1 + restitution) * min(dot(v, n), 0)))
        nv = velocity_after(v, drag, gravity, dt - upper)
        np = add(p, mul(add(v, nv), .5 * (dt - upper)))
        ground = floor(np[0], np[2])
        if np[1] <= ground:
            np = (np[0], ground, np[2])
            n = normal(np[0], np[2])
            nv = add(nv, mul(n, -min(dot(nv, n), 0)))
    return np, nv


class ParticleTerrainCollision(unittest.TestCase):
    def assertVectorNear(self, actual, expected, places=9):
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=places)

    def test_flat_disabled_delegates_without_height_or_origin(self):
        def forbidden(*_):
            self.fail('disabled terrain queried height')
        p, v = integrate((0, .1, 0), (2, -3, 1), .1, forbidden,
                         enabled=False, origin=(2, 5, 7), gravity=0)
        self.assertVectorNear(p, (.2, .1, .1))
        self.assertVectorNear(v, (2, 1.5, 1))

    def test_high_arc_skips_height_and_keeps_free_flight_drag(self):
        def forbidden(*_):
            self.fail('safe high arc queried height')
        p, v = (0, 10, 0), (3, -1, 2)
        actual = integrate(p, v, 1/60, forbidden, drag=.7)
        nv = velocity_after(v, .7, -9.81, 1/60)
        self.assertEqual(actual, (add(p, mul(add(v, nv), .5/60)), nv))

    def test_fast_segment_hits_narrow_intervening_ridge(self):
        def ridge(x, _):
            return .12 * max(0, 1 - (x / .024) ** 2) ** 2
        self.assertEqual(ridge(-.2, 0), ridge(.2, 0))
        p, v = integrate((-.2, .06, 0), (24, 0, 0), 1/60, ridge, gravity=0)
        self.assertLess(v[0], 24)
        self.assertGreater(v[1], 0)
        self.assertGreaterEqual(p[1], ridge(p[0], p[2]) - 1e-9)

    def test_slope_reflects_normal_and_preserves_tangent(self):
        incoming = (1, -2, 0)
        _, outgoing = integrate((0, .15, 0), incoming, .1, lambda x, z: .2*x,
                                gravity=0, restitution=.4)
        n, t = (-.2, 1, 0), (1, .2, 0)
        self.assertAlmostEqual(dot(outgoing, n), -.4 * dot(incoming, n))
        self.assertAlmostEqual(dot(outgoing, t), dot(incoming, t))

    def test_initial_penetration_and_resting_tangent_contact(self):
        p, v = integrate((0, -.1, 0), (0, 0, 0), 1/60, lambda x, z: .1, gravity=0, radius=.03)
        self.assertVectorNear(p, (0, .13, 0))
        self.assertVectorNear(v, (0, 0, 0))
        p, v = (0, .03, 0), (1, .2, 0)
        for _ in range(60):
            p, v = integrate(p, v, 1/60, lambda x, z: .2*x, radius=.03, restitution=0)
            self.assertGreaterEqual(p[1], .2*p[0] + .03 - 1e-9)
            self.assertGreaterEqual(dot(v, (-.2, 1, 0)), -1e-9)

    def test_scene_origin_and_radius_are_applied_once(self):
        height = lambda x, z: .05 + .02*x + .01*z
        world, velocity = (2, .3, 3), (0, -2, 0)
        origin = (1, 5, -4)
        expected = integrate(world, velocity, .1, height, radius=.02, plane=.1, gravity=0)
        actual = integrate(add(world, mul(origin, -1)), velocity, .1, height,
                           radius=.02, plane=.1, gravity=0, origin=origin)
        self.assertVectorNear(add(actual[0], origin), expected[0])
        self.assertVectorNear(actual[1], expected[1])

    def test_upward_curved_arc_checks_its_interior(self):
        calls = []
        p, v = integrate((0, .6, 0), (0, -30, 0), .04,
                         lambda x, z: calls.append(1) or .4, gravity=1500)
        self.assertTrue(calls)
        self.assertGreater(v[1], 30)
        self.assertGreaterEqual(p[1], .4)

    def test_authored_bound_and_shader_routing(self):
        source = json.loads((ROOT/'ContentSource/Models/Environment/meadow_surface.json').read_text())
        relief = source['relief']
        bound = abs(relief['broad_amplitude']) + abs(relief['meso_amplitude'])
        bound += max(r['depth'] for r in source['records'] if r['kind'] == 0) * relief['rut_bank_scale']
        self.assertLess(bound, .55)
        shader = (ROOT/'Content/Shaders/particle_collision.hlsli').read_text()
        self.assertIn('IntegrateFlatBallisticStep(particle, dt);', shader)
        self.assertIn('MeadowGroundHeight(position + ParticleTerrain.xz) - ParticleTerrain.y', shader)
        self.assertIn('refine < 6u', shader)
        self.assertIn('1.0, 64.0', shader)
        self.assertIn('shortcut:', shader)


if __name__ == '__main__':
    unittest.main()
