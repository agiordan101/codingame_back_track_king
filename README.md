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

### Heuristic

Dans un beam search, l'heuristic permet de comparer des état ayant le même état parent. Ces état viennent d'avoir leur score update siute au tour.
Donc un état qui créer de meilleur connection pour moi ou casse des connectino pour l'adversaire va impacter en conséquence les points.
Ce qui veut dire qu'on a pas besoin dans l'heuristic de récompenser/malus ses rails et les rails de l'adveraire sur les chemins les plus court existants.

Il faut juste diriger l'algo vers la création de ces chemins.
Pour ça on veut juste savoir les gap de distance entre les groupes de rails lié à 2 towns qui veulent être lié.

Si l'heuristic reste très basique alors on peut faire plus de simulation, et donc s'orienter vers des cas où les points augmente.

### Idées

#### GA pour construire un graph pondéré

Trouver la longeur des chemins de rails les plus court entre chaque ville (matrice de taille NbVille * NbVille)
Créer un graph pondéré avec comme configuration par défault les liaisons de ville demandé.
Faie un GA qui va couper et créer des liaisons pondéré pour minimiser la distance totale de TOUTE les liaisons du graph.
Les graph qui ne posède pas les liaisons de ville demandé doivent être extremement déavantagé.

FONTIONNE PAS :
- Les chemins doivent pouvoir être lié n'importe où, pas que sur des villes
- Construire un graph blobale ne rapporte pas beaucoup de points par rapport à faire pleins de liaisons rapidemment. Trop lent

## Game engine

### Lookup tables

On peut faire une LT qui garde une struct d'info "path" entre 2 cells :
    - Distance A*
    - List de région par lesquelles ont passe

Une structure associé pourrait permettre de retrouver tous les path qui passe par une région. Créé en même temps
De cette manière, lorsque la région est inked, on peut recalculer tous les paths qui l'utilisait


### Cache A* results instead of lookup tables

Each time we want a A* distance, verify if a cache entry exist :
- If so, verify the inkedRegion count is the same as the cached value :
    - If so, return it
    - Else, compute A* distance, save in cache with inkedRegion count
- If not, compute A* distance, save in cache with inkedRegion count

## Next steps

- Améliorer le PROFILE pour avoir la moyenne par tour de jeu
- openGapTotal prends 1/2 du temps total.. Supprimer entierement et refaire le cache a* avec invalidation quand région supprimé.
- Lister les endroits ou on fait des floodfill/a* et mettre en cache tout ça
- Eviter les cross-products
- Créer une heuristic
- Ne pas créer des moves uniquement sur les groupe de rails des villes pas encore lié :
    - Creer des moves sur les fin de chemin vers le rail le plus proche qui n'est pas du même groupe 

- Il faut qu'un choix de placement soit un ensemble de 3 rails et pas juste une src/dst
    - Des fois on veut 1 ou 2 rails sur la tache principale, et commencer immédiatement une autre tache
    -> Une depth de beam devrait être 1 rail

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
