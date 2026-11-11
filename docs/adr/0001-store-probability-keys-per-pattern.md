# Store each pattern's probability key, so a restored pattern rolls the same as its source

Probability rolls used the pattern index, so restoring a commit into a new pattern would change every roll. We store each pattern's probability key in the engine and in the source snapshot, and restore writes the key back. We accepted the cost of a change to the engine's state and snapshot format, because restore must sound identical to the original and the spec requires probability keys to survive capture.
