-----------------------------------
-- Field Manual locations (server_ffxi)
-----------------------------------
-- Data shared by nomad_moogles.lua and city_teleporter.lua: each Field Manual gets a Nomad
-- Moogle on one side and a City Teleporter on the other, and is a teleporter destination.
-- Not a module (not listed in modules/init.txt); the two modules require() it.
--
-- The positions are the Field Manual NPCs of data/zones/<zone>/npcs.yaml, grouped the way
-- the teleporter menu shows them: area > zone > manual. `label` tells a zone's manuals
-- apart (where it is relative to the zone's other manuals); '' when the zone has one.
-- `placed = true`: a set placed by hand already stands by this manual (the `spots` lists in
-- the two modules), so none is added, and `arrive` is where the teleporter drops players.
--
-- Changes to this file need an xi_map restart (require() caches it).
-----------------------------------
local fieldManuals = {}

fieldManuals.areas =
{
    {
        name  = 'Ronfaure / Norvallen',
        zones =
        {
            {
                folder  = 'West_Ronfaure',
                name    = 'West Ronfaure',
                manuals =
                {
                    { label = 'Northeast', x = -163.752, y = -60.000, z = 295.530, rot = 217 },
                    { label = 'Southwest', x = -450.649, y = -19.800, z = -223.387, rot = 126 },
                },
            },
            {
                folder  = 'East_Ronfaure',
                name    = 'East Ronfaure',
                manuals =
                {
                    { label = 'North', x = 84.802, y = -59.906, z = 240.052, rot = 162 },
                    { label = 'South', x = 283.348, y = -16.548, z = -255.910, rot = 183 },
                },
            },
            {
                folder  = 'Jugner_Forest',
                name    = 'Jugner Forest',
                manuals =
                {
                    { label = 'Northeast', x = 65.273, y = 0.398, z = -14.112, rot = 60 },
                    { label = 'Southwest', x = -560.862, y = -0.444, z = -445.029, rot = 0 },
                },
            },
            {
                folder  = 'Batallia_Downs',
                name    = 'Batallia Downs',
                manuals =
                {
                    { label = 'West', x = -413.391, y = -9.197, z = -227.871, rot = 159 },
                    { label = 'East', x = 438.355, y = 8.932, z = -141.327, rot = 180 },
                },
            },
        },
    },
    {
        name  = 'Zulkheim',
        zones =
        {
            {
                folder  = 'La_Theine_Plateau',
                name    = 'La Theine Plateau',
                manuals =
                {
                    { label = 'Northwest', x = -576.994, y = -4.033, z = 661.371, rot = 148, placed = true, arrive = { -560.711, 0.992, 663.137, 65 } },
                    { label = 'Southeast', x = 437.557, y = 23.999, z = 3.054, rot = 63 },
                },
            },
            {
                folder  = 'Valkurm_Dunes',
                name    = 'Valkurm Dunes',
                manuals =
                {
                    { label = 'West', x = 133.164, y = -7.463, z = 96.594, rot = 149 },
                    { label = 'Northeast', x = 754.167, y = -8.858, z = 388.238, rot = 122 },
                    { label = 'Southeast', x = 905.900, y = -0.638, z = -410.649, rot = 142 },
                },
            },
            {
                folder  = 'Konschtat_Highlands',
                name    = 'Konschtat Highlands',
                manuals =
                {
                    { label = 'Southwest', x = -285.839, y = -48.539, z = -421.089, rot = 105 },
                    { label = 'Northeast', x = 203.023, y = 23.999, z = 283.554, rot = 61 },
                },
            },
        },
    },
    {
        name  = 'Gustaberg / Derfland',
        zones =
        {
            {
                folder  = 'North_Gustaberg',
                name    = 'North Gustaberg',
                manuals =
                {
                    { label = 'East', x = 625.019, y = -1.045, z = 316.154, rot = 56 },
                    { label = 'Southwest', x = -596.486, y = 40.213, z = 57.049, rot = 28 },
                    { label = 'North', x = 166.759, y = -0.438, z = 693.964, rot = 255 },
                },
            },
            {
                folder  = 'South_Gustaberg',
                name    = 'South Gustaberg',
                manuals =
                {
                    { label = 'East', x = 550.415, y = -0.933, z = -315.455, rot = 196 },
                    { label = 'West', x = 49.825, y = -0.411, z = -457.071, rot = 234 },
                },
            },
            {
                folder  = 'Pashhow_Marshlands',
                name    = 'Pashhow Marshlands',
                manuals =
                {
                    { label = 'Northeast', x = 477.439, y = 24.648, z = 419.927, rot = 29 },
                    { label = 'Southwest', x = -536.418, y = 24.999, z = -632.179, rot = 247 },
                },
            },
            {
                folder  = 'Rolanberry_Fields',
                name    = 'Rolanberry Fields',
                manuals =
                {
                    { label = 'South', x = -93.268, y = -1.614, z = -903.464, rot = 166 },
                    { label = 'North', x = 200.404, y = 24.256, z = 489.175, rot = 114 },
                },
            },
        },
    },
    {
        name  = 'Sarutabaruta / Kolshushu',
        zones =
        {
            {
                folder  = 'West_Sarutabaruta',
                name    = 'West Sarutabaruta',
                manuals =
                {
                    { label = 'East', x = 289.632, y = -4.864, z = -5.926, rot = 78 },
                    { label = 'South', x = 147.968, y = -0.647, z = -313.836, rot = 227 },
                    { label = 'Northwest', x = -9.975, y = -12.375, z = 322.987, rot = 0 },
                },
            },
            {
                folder  = 'East_Sarutabaruta',
                name    = 'East Sarutabaruta',
                manuals =
                {
                    { label = 'South', x = -108.115, y = -4.649, z = -526.442, rot = 103 },
                    { label = 'Northwest', x = -400.403, y = -25.063, z = 608.558, rot = 189 },
                    { label = 'East', x = 384.000, y = -13.250, z = 94.000, rot = 0 },
                },
            },
            {
                folder  = 'Tahrongi_Canyon',
                name    = 'Tahrongi Canyon',
                manuals =
                {
                    { label = 'South', x = -88.305, y = 46.963, z = -720.889, rot = 126 },
                    { label = 'North', x = 117.676, y = 39.999, z = 358.262, rot = 253 },
                    { label = 'East', x = 319.094, y = -16.983, z = -88.081, rot = 56 },
                },
            },
            {
                folder  = 'Buburimu_Peninsula',
                name    = 'Buburimu Peninsula',
                manuals =
                {
                    { label = 'West', x = -474.500, y = -32.206, z = 47.284, rot = 61 },
                    { label = 'East', x = 147.125, y = -1.017, z = -185.507, rot = 117 },
                },
            },
        },
    },
    {
        name  = 'Aragoneu / Qufim',
        zones =
        {
            {
                folder  = 'Meriphataud_Mountains',
                name    = 'Meriphataud Mountains',
                manuals =
                {
                    { label = 'Northwest', x = -298.685, y = 17.047, z = 425.156, rot = 219 },
                    { label = 'South', x = -124.532, y = -25.108, z = -590.308, rot = 213 },
                    { label = 'East', x = 609.155, y = -33.182, z = 256.742, rot = 242 },
                },
            },
            {
                folder  = 'Sauromugue_Champaign',
                name    = 'Sauromugue Champaign',
                manuals =
                {
                    { label = 'Southeast', x = 472.422, y = -8.864, z = -440.169, rot = 125 },
                    { label = 'Northwest', x = -458.391, y = 0.918, z = 359.792, rot = 227 },
                },
            },
            {
                folder  = 'Qufim_Island',
                name    = 'Qufim Island',
                manuals =
                {
                    { label = 'Northwest', x = -257.517, y = -20.000, z = 299.657, rot = 65 },
                    { label = 'Southeast', x = 179.180, y = -20.791, z = -25.059, rot = 56 },
                },
            },
            {
                folder  = 'Behemoths_Dominion',
                name    = 'Behemoth\'s Dominion',
                manuals =
                {
                    { label = '', x = 337.466, y = 25.967, z = -63.428, rot = 150 },
                },
            },
        },
    },
    {
        name  = 'Northlands',
        zones =
        {
            {
                folder  = 'Beaucedine_Glacier',
                name    = 'Beaucedine Glacier',
                manuals =
                {
                    { label = 'South', x = -25.207, y = -59.324, z = -123.423, rot = 34 },
                    { label = 'Northeast', x = 105.000, y = -20.250, z = 146.000, rot = 192 },
                    { label = 'East', x = 254.000, y = -0.250, z = -15.000, rot = 128 },
                    { label = 'West', x = -335.000, y = -100.250, z = 146.000, rot = 192 },
                },
            },
            {
                folder  = 'Xarcabard',
                name    = 'Xarcabard',
                manuals =
                {
                    { label = 'West', x = 200.884, y = -23.895, z = -205.316, rot = 177 },
                    { label = 'East', x = 571.209, y = -1.108, z = -262.822, rot = 157 },
                },
            },
        },
    },
    {
        name  = 'Kuzotz / Vollbow',
        zones =
        {
            {
                folder  = 'Eastern_Altepa_Desert',
                name    = 'Eastern Altepa Desert',
                manuals =
                {
                    { label = 'East', x = 472.690, y = -1.165, z = -63.307, rot = 66 },
                    { label = 'Southwest', x = -259.317, y = 8.527, z = -260.421, rot = 149 },
                    { label = 'Northwest', x = -64.828, y = 3.949, z = 232.528, rot = 120 },
                },
            },
            {
                folder  = 'Western_Altepa_Desert',
                name    = 'Western Altepa Desert',
                manuals =
                {
                    { label = 'Northeast', x = 446.106, y = -1.824, z = 353.842, rot = 189 },
                    { label = 'Northwest', x = -216.138, y = -11.805, z = 102.130, rot = 41 },
                    { label = 'South', x = -113.032, y = -2.101, z = -474.046, rot = 65 },
                },
            },
            {
                folder  = 'Cape_Teriggan',
                name    = 'Cape Teriggan',
                manuals =
                {
                    { label = 'Southwest', x = -187.696, y = 8.078, z = -70.850, rot = 129 },
                    { label = 'Northeast', x = 198.843, y = -0.253, z = 377.029, rot = 70 },
                },
            },
            {
                folder  = 'Valley_of_Sorrows',
                name    = 'Valley of Sorrows',
                manuals =
                {
                    { label = 'West', x = -160.565, y = -8.283, z = 23.096, rot = 192 },
                    { label = 'East', x = 136.458, y = -8.431, z = -76.994, rot = 139 },
                },
            },
        },
    },
    {
        name  = 'Elshimo',
        zones =
        {
            {
                folder  = 'Yuhtunga_Jungle',
                name    = 'Yuhtunga Jungle',
                manuals =
                {
                    { label = 'North', x = -224.526, y = -0.298, z = 497.211, rot = 42 },
                    { label = 'South', x = -236.903, y = 0.000, z = -403.785, rot = 64 },
                    { label = 'Southwest', x = -457.094, y = 8.102, z = -38.796, rot = 243 },
                    { label = 'Northeast', x = 380.648, y = 20.113, z = 348.324, rot = 12 },
                },
            },
            {
                folder  = 'Yhoator_Jungle',
                name    = 'Yhoator Jungle',
                manuals =
                {
                    { label = 'East', x = 194.933, y = 0.000, z = -83.997, rot = 157 },
                    { label = 'Southwest', x = -280.777, y = 0.267, z = -152.416, rot = 50 },
                    { label = 'Northwest', x = -272.773, y = 6.644, z = 142.934, rot = 193 },
                },
            },
        },
    },
    {
        name  = 'Li\'Telor / Tu\'Lia',
        zones =
        {
            {
                folder  = 'The_Sanctuary_of_ZiTah',
                name    = 'Sanctuary of Zi\'Tah',
                manuals =
                {
                    { label = 'Southwest', x = -461.454, y = 0.990, z = -461.800, rot = 125 },
                    { label = 'Northeast', x = -34.000, y = 0.057, z = -150.177, rot = 67 },
                },
            },
            {
                folder  = 'RoMaeve',
                name    = 'Ro\'Maeve',
                manuals =
                {
                    { label = 'South', x = 7.137, y = -8.000, z = -143.992, rot = 61 },
                    { label = 'North', x = -13.327, y = -28.000, z = 50.600, rot = 85 },
                },
            },
            {
                folder  = 'RuAun_Gardens',
                name    = 'Ru\'Aun Gardens',
                manuals =
                {
                    { label = 'South', x = -15.186, y = -54.159, z = -591.972, rot = 190 },
                    { label = 'Southeast', x = 312.271, y = -42.010, z = -421.380, rot = 223 },
                    { label = 'Northeast', x = 497.143, y = -42.009, z = 166.642, rot = 174 },
                    { label = 'North', x = -4.805, y = -42.011, z = 524.558, rot = 127 },
                    { label = 'Northwest', x = -500.333, y = -42.010, z = 157.631, rot = 73 },
                    { label = 'Southwest', x = -304.365, y = -42.010, z = -427.083, rot = 24 },
                },
            },
        },
    },
}

local sideDistance = 1.5 -- yalms from the manual to the NPC beside it

-- Where the NPC on one side of a manual stands. side: 1 or -1 (the two sides of the way the
-- manual faces). Tries straight to the side, then diagonally in front and behind, and takes
-- the first point the zone's navmesh says can be stood on; straight to the side if none.
-- Returns x, y, z, rotation.
fieldManuals.beside = function(zone, manual, side)
    local facing = manual.rot / 256 * 2 * math.pi -- 0 = east, clockwise; z grows north
    local first  = nil

    for _, turn in ipairs({ 0.25, 0.125, 0.375 }) do
        local angle = facing + side * turn * 2 * math.pi
        local x     = manual.x + math.cos(angle) * sideDistance
        local z     = manual.z - math.sin(angle) * sideDistance

        first = first or { x, z }
        if zone:isNavigablePoint({ x = x, y = manual.y, z = z }) then
            return x, manual.y, z, manual.rot
        end
    end

    return first[1], manual.y, first[2], manual.rot
end

-- Calls fn(zoneEntry, manual, index) for every manual; index counts within the zone.
fieldManuals.each = function(fn)
    for _, area in ipairs(fieldManuals.areas) do
        for _, zoneEntry in ipairs(area.zones) do
            for i, manual in ipairs(zoneEntry.manuals) do
                fn(zoneEntry, manual, i)
            end
        end
    end
end

return fieldManuals
