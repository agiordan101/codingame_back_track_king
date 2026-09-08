# codingame_back_track_king

CodinGame Summer Challenge 2026

## Chosen algorithms

### BEAM search

#### "Rail placement choice"s création

1. Select all desired connection not yet built
2. For each of those
    - Find and create two rails group that are connected to the towns
    - Iterate over cross-product of those two groups to identify all potentials connection, and keep the shortest one (using manhattan distance)
3. We end up with all shortest path to build desired connections. A paht being 2 tiles/Coordonates

#### "Disrupt choice" création

All régions which respect all conditions :
- Not inked
- No town
- Opponent rail
- For each unique region rail within a valid connection :
    - Count all unique player rails inside the connection
- Sum rails count for both players
- Keep regions where opponent has a bigger count of rails than mine

Create only the best one

#### "Rail placement choice" application

Rule to place one player rails :

We want to apply a path/"rail placement choice". We get 2 coordonate, the first being the source and second the destination
The idea is to check around the source (4 adjacents cells) the closest cell (using A*) to the destination.
If draw, prioritize in order with :
- NORTH
- EAST
- SOUTH
- WEST
Once it's done, check the cost of the rail placement :
- 1 point de peinture pour placer un rail sur les plaines.
- 2 points de peinture pour placer un rail sur une rivière.
- 3 points de peinture pour placer un rail sur les montagnes.

and repeat until 3 points are spent.

Then add rails onto map. Be careful, both player must add they rails at the same time

#### Game engine turn choices application

- Start with state D
- Pick one of my rails creation
- Pick one of opp rails creation
- Apply both rails creation
- Apply chosen disrupts
- Ink regions
- Compute new player points
- End with state D+1

#### BEAM iterations

1. Start with parameter current_state
2. Copy current_state in turn_state
3. Generate rail choices with section "Rail placement choices création"
4. Generate my best disrupt choices with section "Disrupt choices"
4. Generate opp best disrupt choices with section "Disrupt choices"
7. Iterate over rails choices
   - Copy turn_state in current_state
   - Create state D+1 with section "Game engine turn choices application"
   - Use heuristic function to evaluate state D+1
   - Keep the state D+1 if heuristic score if best than current Bwidth lowest one
8. End with Bwidth new states

Bwidth = 20
maxDepth=10


## Game engine

### Lookup tables

On peut faire une LT qui garde une struct d'info "path" entre 2 cells :
    - Distance A*
    - List de région par lesquelles ont passe

Une structure associé pourrait permettre de retrouver tous les path qui passe par une région. Créé en même temps
De cette manière, lorsque la région est inked, on peut recalculer tous les paths qui l'utilisait

## Next steps

- Créer les actions avec tous les groupes de rails >= 3 plutot que ceux directement lié aux towns ?

## Explanations

## Versions

### v2.0

* Rail placement choices : for each unbuilt desired connection, flood the rail/town group attached to each town, cross-product both groups, keep the shortest Manhattan link.
* Disrupt choice : regions that are not inked, hold no town, carry opponent rails, and where the opponent's unique connection-rail count exceeds mine; best one only.
* Rail application : walks source→destination picking the neighbour closest to the target, ties broken NORTH/EAST/SOUTH/WEST, spending exactly the 3 paint points at 1/2/3 per plains/river/mountain.
* Turn simulation : both players' rails applied simultaneously (shared tile → neutral owner 2), then disrupts, then inking.
* Beam search : Bwidth 20, maxDepth 10, heuristic = my connection points − opponent's.


### v1.2

### v1.1

### v1.0

- Find shortest distance amongs desired connections to build
- Skip connection if active
- Pick which region to disrupt: one with enemy rails, not yet inked,
    not containing one of our/their towns (can't disrupt those),
    preferring the one closest to being inked / with the most rails

Last moment in arena: 
First moment in arena: 191/558 Bronze

### v0.2

Last moment in arena: 320/558 Bronze
First moment in arena: -
