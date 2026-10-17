-- Run from a temporary working directory: lua5.4 /path/to/test_neural.lua /path/to/plugins/brain.lua
bot = { call = function() return {} end, tool = function() end, every = function() end }
assert(loadfile(arg[1]))()
local net = NN.new(3, 4, 2, 42)
local x = { 1, 0.3, -0.2 }
local p, _, value, layers = NN.forward(net, x)
assert(#layers == 5 and #net.layers == 4, "five hidden layers")
local legacy = NN.copy(net)
legacy.layers = nil
local migrated = NN.grow(legacy, 5, 3)
for h = 1, 4 do
    assert(migrated.w1[h][4] == 0 and migrated.w1[h][5] == 0, "new inputs start neutral")
    assert(migrated.w2[1][h] == net.w2[1][h], "saved action weights survive growth")
end
local upgraded = NN.deepen(legacy)
local q, _, v = NN.forward(upgraded, x)
for i = 1, #p do assert(math.abs(p[i] - q[i]) < 1e-12, "deepening preserves the old policy") end
assert(v == value)
-- Compare backpropagation with a numerical policy/entropy gradient at every layer.
local function objective(n)
    local probabilities = NN.forward(n, x)
    local score = math.log(probabilities[1])
    for _, probability in ipairs(probabilities) do score = score - 0.2 * probability * math.log(probability + 1e-12) end
    return score
end
for _, field in ipairs({ "input", "middle", "last", "output" }) do
    local function row(n)
        if field == "input" then return n.w1[1] end
        if field == "output" then return n.w2[1] end
        return n.layers[field == "middle" and 1 or #n.layers].w[1]
    end
    local plus, minus, trained = NN.copy(net), NN.copy(net), NN.copy(net)
    row(plus)[1], row(minus)[1] = row(plus)[1] + 1e-5, row(minus)[1] - 1e-5
    local expected = (objective(plus) - objective(minus)) / 2e-5
    NN.learn(trained, x, 1, 1)
    assert(math.abs((row(trained)[1] - row(net)[1]) / 0.1 - expected) < 1e-6, field .. " gradient")
end
local before = NN.copy(net)
NN.learn(net, x, 1, 1)
assert(net.w1[1][1] ~= before.w1[1][1], "input layer learns")
assert(net.w2[1][1] ~= before.w2[1][1], "output layer learns")
for l = 1, 4 do
    assert(net.layers[l].w[1][1] ~= before.layers[l].w[1][1], "every added layer learns")
    assert(before.layers[l].w[1][1] == 0, "copies do not alias layers")
end
for i = 1, 1000 do NN.learn(net, x, 1, i % 2 == 0 and 1e6 or -1e6) end
local probabilities, _, critic = NN.forward(net, { 1e6, -1e6, 1e6 })
local sum = 0
for _, n in ipairs(probabilities) do assert(n == n and n >= 0 and n <= 1); sum = sum + n end
assert(math.abs(sum - 1) < 1e-12 and critic == critic and math.abs(critic) < math.huge, "finite normalized predictions")
local mutated = NN.mutated(before)
assert(mutated.layers[1].w[1][1] ~= before.layers[1].w[1][1], "evolution includes added layers")
print("neural OK: migration, independent copies, all-layer learning, stable predictions, evolution")
-- Both elites survive unchanged; every other bot gets a separate mutated copy of either parent.
math.randomseed(42)
local population, fitness = {}, {}
for i, name in ipairs({ "A", "B", "C", "D" }) do
    population[name] = NN.new(3, 4, 2, i)
    population[name].vb = i * 100
    fitness[name] = { 5 - i, 1 }
end
local a, b = NN.serialize(population.A), NN.serialize(population.B)
local ranking, parents = NN.evolve(population, fitness)
assert(parents[1] == "A" and parents[2] == "B" and #ranking == 4, "two best parents")
assert(NN.serialize(population.A) == a and NN.serialize(population.B) == b, "both elites stay unchanged")
assert(population.C.vb == 100 and population.D.vb == 200, "offspring alternate parents")
assert(population.C.w1[1][1] ~= population.A.w1[1][1] and population.D.w1[1][1] ~= population.B.w1[1][1], "offspring have mutations")
population.C.w1[1][1] = 999
assert(population.A.w1[1][1] ~= 999 and population.D.w1[1][1] ~= 999, "offspring never alias")
local empty, none = NN.evolve(population, {})
assert(#empty == 0 and #none == 0, "no winner without evaluated experience")
print("evolution OK: two elites, independent mutated offspring, unevaluated policies excluded")
