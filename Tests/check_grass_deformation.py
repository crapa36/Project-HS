"""Numeric grass contract: conservative candidates, old position parity, exact local Jacobian.

Shader compilation/reflection is covered by the renderer shader gate. These tests
compare the candidate/Jacobian model to an independent full-loop position oracle
and central differences; they do not require a GPU or third-party Python packages.
"""
import math
import random
import struct
import unittest


def f32(value):
    try:
        return struct.unpack("f", struct.pack("f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def mul(a, value): return tuple(x * value for x in a)
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def unit(a): return mul(a, 1 / math.sqrt(dot(a, a)))
def saturate(value): return min(1., max(0., value))


def build_grid(benders):
    benders = benders[:32]
    disabled = ((0., 0., 0., 0.), [0] * 64)
    if len(benders) <= 3 or any(not math.isfinite(v) for b in benders for v in b):
        return disabled
    bounds = [(b[0]-max(b[2], f32(.1)), b[1]-max(b[2], f32(.1)),
               b[0]+max(b[2], f32(.1)), b[1]+max(b[2], f32(.1))) for b in benders]
    low_x, low_z = min(b[0] for b in bounds), min(b[1] for b in bounds)
    extent = max(max(b[2] for b in bounds)-low_x, max(b[3] for b in bounds)-low_z)
    if not extent > 0: return disabled
    grid = (f32(low_x), f32(low_z), f32(8 / extent), 1.)
    if any(not math.isfinite(v) for v in grid) or grid[2] < 2**-126: return disabled
    masks = [0] * 64
    def cell(value, origin, padding):
        return int(min(7., max(0., math.floor((value-origin)*grid[2])+padding)))
    for index, bounds in enumerate(bounds):
        for z in range(cell(bounds[1], grid[1], -1), cell(bounds[3], grid[1], 1)+1):
            for x in range(cell(bounds[0], grid[0], -1), cell(bounds[2], grid[0], 1)+1):
                masks[z*8+x] |= 1 << index
    return grid, masks


def candidate_mask(world, benders, encoded):
    grid, masks = encoded
    count = min(len(benders), 32)
    all_benders = (1 << count) - 1
    if count <= 3 or grid[3] < .5: return all_benders
    cell = (f32(f32(world[0]-grid[0])*grid[2]), f32(f32(world[2]-grid[1])*grid[2]))
    if any(c < 0 or c >= 8 for c in cell): return all_benders
    return masks[int(cell[1])*8+int(cell[0])] & all_benders


def old_position(world, local_y, time, benders):
    if local_y <= -.45: return world
    height = saturate(local_y+.5)
    bend = (0., 0., 0.)
    for bx, bz, radius in benders[:32]:
        dx, dz = world[0]-bx, world[2]-bz
        radius = max(radius, f32(.1))
        if max(abs(dx), abs(dz)) > 2*radius: continue
        weight = saturate(1-math.hypot(dx, dz)/radius)
        bend = add(bend, tuple(v*weight*.42*height for v in (dx, 0., dz)))
    wind = math.sin(time*1.7+world[0]*.11+world[2]*.07)*.035
    return add(add(world, bend), mul((wind, 0., wind*.7), height))


def deform(world, local_y, height_gradient, time, benders, encoded):
    columns = [(1., 0., 0.), (0., 1., 0.), (0., 0., 1.)]
    if local_y <= -.45: return world, columns
    height = saturate(local_y+.5)
    bend, height_displacement = (0., 0., 0.), (0., 0., 0.)
    candidates = candidate_mask(world, benders, encoded)
    while candidates:
        bit = candidates & -candidates
        index = bit.bit_length()-1
        candidates &= candidates-1
        bx, bz, radius = benders[index]
        dx, dz = world[0]-bx, world[2]-bz
        radius = max(radius, f32(.1))
        if max(abs(dx), abs(dz)) > radius: continue
        distance = math.hypot(dx, dz)
        weight = saturate(1-distance/radius)
        displacement = tuple(v*weight*.42 for v in (dx, 0., dz))
        bend = add(bend, mul(displacement, height))
        height_displacement = add(height_displacement, displacement)
        if distance < radius:
            radial = 1/(radius*distance) if distance else 0.
            xx, xz, zz = .42*(weight-dx*dx*radial), -.42*dx*dz*radial, .42*(weight-dz*dz*radial)
            columns[0] = add(columns[0], mul((xx, 0., xz), height))
            columns[2] = add(columns[2], mul((xz, 0., zz), height))
    phase = time*1.7+world[0]*.11+world[2]*.07
    wind, wind_gradient = math.sin(phase)*.035, math.cos(phase)*.035
    wind_displacement = (wind, 0., wind*.7)
    position = add(add(world, bend), mul(wind_displacement, height))
    columns[0] = add(columns[0], mul((1., 0., .7), wind_gradient*.11*height))
    columns[2] = add(columns[2], mul((1., 0., .7), wind_gradient*.07*height))
    height_displacement = add(height_displacement, wind_displacement)
    for axis in range(3):
        columns[axis] = add(columns[axis], mul(height_displacement, height_gradient[axis] if local_y < .5 else 0.))
    return position, columns


def frame(columns, normal, tangent, handedness=1.):
    dx, dy, dz = columns
    cofactor_x = cross(dy, dz)
    determinant = dot(dx, cofactor_x)
    if not math.isfinite(determinant) or abs(determinant) < 1e-6: return normal, tangent, handedness
    bent_normal = mul(add(add(mul(cofactor_x, normal[0]), mul(cross(dz, dx), normal[1])),
                          mul(cross(dx, dy), normal[2])), -1. if determinant < 0 else 1.)
    length = dot(bent_normal, bent_normal)
    if not math.isfinite(length) or length < 1e-12: return normal, tangent, handedness
    bent_normal = unit(bent_normal)
    bent_tangent = add(add(mul(dx, tangent[0]), mul(dy, tangent[1])), mul(dz, tangent[2]))
    bent_tangent = add(bent_tangent, mul(bent_normal, -dot(bent_normal, bent_tangent)))
    length = dot(bent_tangent, bent_tangent)
    if not math.isfinite(length) or length < 1e-12: return normal, tangent, handedness
    return bent_normal, unit(bent_tangent), handedness*(-1 if determinant < 0 else 1)


class GrassDeformationTests(unittest.TestCase):
    def assert_vector(self, actual, expected, tolerance=1e-9):
        self.assertLessEqual(max(abs(a-b) for a, b in zip(actual, expected)), tolerance)

    def test_candidates_positions_and_radial_boundaries(self):
        rng = random.Random(82731)
        for count in (0, 1, 3, 4, 8, 32):
            for origin in ((0., 0.), (173.25, -91.75)):
                benders = [(f32(rng.uniform(-18, 18)+origin[0]), f32(rng.uniform(-18, 18)+origin[1]),
                            f32(rng.uniform(.01, 2))) for _ in range(count)]
                encoded = build_grid(benders)
                points = [(rng.uniform(-25, 25)+origin[0], rng.uniform(-1, 3), rng.uniform(-25, 25)+origin[1])
                          for _ in range(180)]
                for bx, bz, radius in benders:
                    for scale in (0., 1-1e-6, 1., 1+1e-6, 2.):
                        points.append((bx+max(radius, f32(.1))*scale, .4, bz))
                grid = encoded[0]
                if grid[3]:
                    for boundary in range(9):
                        x = grid[0]+boundary/grid[2]
                        points.extend([(math.nextafter(x, side), 0., grid[1]+3.2/grid[2])
                                       for side in (-math.inf, math.inf)])
                for world in points:
                    mask = candidate_mask(world, benders, encoded)
                    for index, (bx, bz, radius) in enumerate(benders):
                        if math.hypot(world[0]-bx, world[2]-bz) < max(radius, f32(.1)):
                            self.assertTrue(mask & (1 << index), (world, index, encoded[0]))
                    for local_y in (-.5, -.45, -.449, .15, .5, 1.2):
                        result, _ = deform(world, local_y, (0., 2., 0.), 2.7, benders, encoded)
                        self.assert_vector(result, old_position(world, local_y, 2.7, benders))
        extreme = [(f32(1e38), f32(1e38), .1)]*4
        self.assertEqual(build_grid(extreme)[0][3], 0.)
        self.assertEqual(build_grid([(math.inf, 0., 1.)]*4)[0][3], 0.)

    def test_previous_origin_uses_independent_grid(self):
        rng = random.Random(621)
        previous = [(f32(rng.uniform(-20, 20)), f32(rng.uniform(-20, 20)), 1.25) for _ in range(32)]
        delta = (13., 0., -9.)
        current = [(x-delta[0], z-delta[2], r) for x, z, r in previous]
        previous_grid, current_grid = build_grid(previous), build_grid(current)
        wrong_grid_misses = 0
        for bx, bz, _ in previous:
            previous_world = (bx+.3, .2, bz+.1)
            current_rebased = add(previous_world, mul(delta, -1))
            reconstructed = add(current_rebased, delta)
            result, _ = deform(reconstructed, .2, (0., 1., 0.), 3.1, previous, previous_grid)
            self.assert_vector(add(result, mul(delta, -1)),
                               add(old_position(previous_world, .2, 3.1, previous), mul(delta, -1)))
            bad, _ = deform(reconstructed, .2, (0., 1., 0.), 3.1, previous, current_grid)
            wrong_grid_misses += max(abs(a-b) for a, b in zip(bad, result)) > 1e-7
        self.assertGreater(wrong_grid_misses, 0)

    def test_jacobian_and_normal_tangent_frame(self):
        rng = random.Random(717)
        for _ in range(160):
            benders = [(rng.uniform(-1.5, 1.5), rng.uniform(-1.5, 1.5), rng.uniform(.3, 2.)) for _ in range(8)]
            world = (rng.uniform(-1, 1), rng.uniform(0, 1), rng.uniform(-1, 1))
            local_y, scale = rng.uniform(-.35, .35), [rng.uniform(.2, 3.) for _ in range(3)]
            yaw, time = rng.uniform(-math.pi, math.pi), rng.uniform(0, 30)
            height_gradient = (0., 1/scale[1], 0.)
            encoded = build_grid(benders)
            _, columns = deform(world, local_y, height_gradient, time, benders, encoded)
            epsilon = 1e-6
            for axis in range(3):
                step = tuple(epsilon if i == axis else 0. for i in range(3))
                plus = old_position(add(world, step), local_y+height_gradient[axis]*epsilon, time, benders)
                minus = old_position(add(world, mul(step, -1)), local_y-height_gradient[axis]*epsilon, time, benders)
                self.assert_vector(columns[axis], mul(add(plus, mul(minus, -1)), .5/epsilon), 2e-6)
            local_normal = unit((rng.uniform(-1, 1), rng.uniform(-1, 1), 1.))
            local_tangent = unit(cross(local_normal, (0., 1., 0.)))
            def rotate(v):
                return (v[0]*math.cos(yaw)+v[2]*math.sin(yaw), v[1], -v[0]*math.sin(yaw)+v[2]*math.cos(yaw))
            normal = unit(rotate(tuple(n/s for n, s in zip(local_normal, scale))))
            tangent = unit(rotate(tuple(t*s for t, s in zip(local_tangent, scale))))
            bent_normal, bent_tangent, handedness = frame(columns, normal, tangent)
            self.assertAlmostEqual(dot(bent_normal, bent_tangent), 0., places=10)
            self.assertAlmostEqual(dot(bent_normal, bent_normal), 1., places=10)
            self.assertAlmostEqual(dot(bent_tangent, bent_tangent), 1., places=10)
            for direction in (tangent, cross(normal, tangent)):
                step = mul(direction, epsilon)
                plus = old_position(add(world, step), local_y+dot(height_gradient, step), time, benders)
                minus = old_position(add(world, mul(step, -1)), local_y-dot(height_gradient, step), time, benders)
                self.assertAlmostEqual(dot(bent_normal, mul(add(plus, mul(minus, -1)), .5/epsilon)), 0., places=5)
            self.assertIn(handedness, (-1., 1.))
        normal, tangent = (0., 0., 1.), (1., 0., 0.)
        self.assertEqual(frame([(1., 0., 0.), (0., 1., 0.), (0., 0., 0.)], normal, tangent), (normal, tangent, 1.))
        self.assertEqual(frame([(-1., 0., 0.), (0., 1., 0.), (0., 0., 1.)], normal, tangent)[2], -1.)
        center = [(0., 0., 1.)]
        _, columns = deform((0., 0., 0.), .2, (0., .5, 0.), 0., center, build_grid(center))
        self.assertTrue(all(math.isfinite(value) for column in columns for value in column))


if __name__ == "__main__":
    unittest.main()