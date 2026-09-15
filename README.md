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

### Pre-heuristic (`extendManhattanGap`)

Le beam interne classe ses candidats avec un terme très rapide, sans BFS ni
flood fill. Deux moitiés, additionnées :

1. **Gaps à fermer** — pour chaque wish pas encore connecté, la marche
   `villeA -> rail -> villeB`, pondérée en `1/baseline²` pour privilégier les
   wishes courts (seuls ceux-là sont finissables en 3 rails, et seule une
   connection finie rapporte). Seuls les deux meilleurs wishes comptent ; un
   wish déjà ponté est mis de côté à sa valeur pleine pour ne pas bloquer un
   slot et aplatir le score des rails suivants.

2. **Chemins qui paient** — une connection active rapporte 1 point par rail
   possédé sur son chemin, *chaque tour*. Les chemins actifs sont tamponnés une
   fois par appel (`payOwner`), puis chaque candidat coûte 4 lectures de
   tableau : un rail adjacent à un chemin qui paie vaut +1, +2 si l'adversaire
   possède cette case (le tie-break N/E/S/O déterministe permet de détourner le
   chemin et de lui prendre le point plutôt que d'en ajouter un).

La moitié 2 est ce qui empêche le terme d'être aveugle quand tous les wishes
sont connectés — l'état dans lequel se passe ~75% d'une partie.

## Next steps

- Tester l'algorithm suivant en greedy first (meilleur combinaison) SANS beam search pour voir ce que ca vaut
- Idées pour le pruning des actions légales du beam search :
    1. Avantages :
        - Priorizer les cases étant sur les chemins les plus courts entre des villes
        - Parmis les connection souhaité, priorizer les connections les plus courtes
        - Prioriser les cases qui sont sur plusieurs chemin les plus courts
        - Prioriser les cases qui ne peuvent pas être supprimé
        - Ecarter les cases qui vont se faire inked
        Algorithme :
        - Chaque main beam node doit re-construire les A* path les plus court entre les villes
        - On repart de la lookup table static ayant les cases uninkable déjà rewarded
        - Pour chaque A* path (Utiliser la récursivité pour ne pas avoir a stocker les coords ?) :
            - On ajoute à toutes les case du path la valeur : w+h-pathLength
        - Pour chaque cases, on multiple la valeur par : (5 - inkLevel) / 5 (CALCUL PROPRE AU PRUNING, l'heuristique devra en avoir un différent: Plus permisiffe)
        - On considère uniquement les cases adjacentes aux rails et villes existants
        - On utilise LA meilleur combinaisons comme actionset de l'adversaire
        - On créé ~10 combinaisons parmis les meilleurs cases comme étant les actionset possible de la node actuel du BEAM SEARCH :
            * En créant toutes les combinaisons des N meilleures cases ? (3 parmis 5) = 10 (Moins en réalité parce que on va des fois avoir que 1 ou 2 rails à poser)
            * Aléatoirement parmis les N meilleurs ? = Plus de varièté et de création de chemins en parralle ?
            Sachant que :
                N=5  -> (3 parmis 5)  =  10 combinaisons
                N=7  -> (3 parmis 7)  =  35 combinaisons
                N=10 -> (3 parmis 10) = 120 combinaisons

- Idées pour l'heuristic principale :
  - Différence de score
  - Nombre de connection active ! Osef parce que ca peut autant être bien pour ladversaire que pour moi
  - sum de 0 ou x si le rail est dans une région inkable (Uninkable regions > inkable regions)
  
- Cache/incrementalize openGapTotal — the single highest-value change. It re-does a full multi-component flood-fill per node when consecutive nodes differ by only ~3 rails.
<!-- - openGapTotal prends 1/2 du temps total.. Supprimer entierement et refaire le cache a* avec invalidation quand région supprimé. -->
<!-- - Lister les endroits ou on fait des floodfill/a* et mettre en cache tout ça -->

### Optimizations list

Here are the optimizations, ranked by expected gain.

3. Avoid constructing children you'll discard
You build all ~1200 children then keep 30. Since evaluate needs the simulated board you can't score-before-build directly, but you can:

Reuse node storage across depths. Keep two vector<BeamNode> buffers as members, clear() instead of reallocating; nextBeam.reserve(beam.size() * NESTED_BEAM_WIDTH) once. Right now nextBeam is constructed fresh per depth and push_back reallocates ~11 times, each realloc moving every node.
Cheap prefilter: myTurns from generateActionSets is already ranked by closedGap. Only the top-K per parent can realistically survive the global cut. If NESTED_BEAM_WIDTH=40 but the global beam is 30, expanding 40 children from each of 30 parents to keep 30 total is heavily wasteful. Try min(NESTED_BEAM_WIDTH, 8) at depth > 0 and measure — you'd likely get 2–3 extra depths for the same budget, which is worth far more than breadth at a single depth.

4. Structural criticism of the search itself
generateActionSets is called twice per node (foe then self), and the foe call only uses foeTurns.front(). That's a full nested beam (NESTED_BEAM_WIDTH=40 lines) to extract one answer. Call it with width 1–4 for the foe. Potentially a ~40% cut of total search time on its own.
The foe plan is recomputed at every node even though sibling nodes at depth 0 share the identical node.state (all children of the same parent see the same board before your rails). Hoist and cache it per parent — you already do, but it's per-node, and at depth 0 there's exactly one node so that's fine; at deeper depths siblings diverge, so nothing to gain there. The width reduction is the real fix.
outOfTime() calls steady_clock::now() per action — that's a vDSO call, ~20–25 ns. With ~1200 actions/depth × 5 depths it's ~150 µs, ~0.3% of a 50 ms budget. Acceptable, but if you shrink the per-child cost as above it becomes relatively significant; check every 8th iteration with a counter mask.
Ordering the merge: at main.cpp:2374 the merged previous beam is already sorted. You could std::merge the sorted old beam with the ranked new children instead of re-ranking everything — minor once the index-sort is in.
Suggested order of work: (2a) shared static Map + Tile shrink, then (1) index-sort, then (4) foe width. (2a) alone should be the multiplier.

## Debug viewer

`tools/` contient une interface qui affiche la map et les valeurs internes de
l'algo : pour chaque case candidate, le `closedGap` que le beam interne lui a
donné — à quel point la ligne sert les deux wishes qu'elle sert le mieux,
pondéré vers les wishes courts, donc plus c'est haut mieux c'est. Une heatmap
uniforme veut dire que tous les wishes sont déjà connectés. `main.cpp` reste compilable et jouable seul — c'est `make check` qui
le garantit, et le binaire de compétition est inchangé au bit près (les hooks
sont des macros vides hors `DEBUG_TOOL`).

### Les 3 heatmaps d'un tour

Le beam interne choisit les 3 rails d'un tour un par un : un round par rail.
Chaque round est capturé séparément, donc un tour porte jusqu'à **3 heatmaps** —
sélectionnables dans le panneau « Rail du tour » (ou touches <kbd>1</kbd>–<kbd>3</kbd>).

Un round score ses cases contre un plateau qui inclut déjà les rails posés par
les rounds précédents. Chaque heatmap affiche donc ce que son préfixe a **déjà
fermé** (`prefixGap`, nul sur le premier rail), et les gains se lisent « ce que
ce rail ajoute au précédent », pas « par rapport au plateau vierge ». Les coups
déjà appliqués (0, 1 ou 2) sont remplis et numérotés sur la carte.

Deux cases seulement sont entourées, celles qui posent la question — et leur
couleur est la réponse :

- **anneau blanc unique** : le rail choisi *est* la meilleure case. Rien à
  expliquer.
- **vert + rouge** : ils divergent. Vert sur le gain maximum (repère en haut à
  gauche), rouge sur le rail réellement choisi (repère en bas à droite).

En vue adversaire il n'y a que l'anneau vert : son coup n'est jamais imprimé.
Sur la partie rejouée, 78 rounds sur 91 divergent et 13 concordent — c'est cet
écart que l'outil sert à regarder.

Le rouge tire vers l'orange à dessein : un rouge pur se confondrait avec les
rails du joueur et avec le haut du gradient rose-rouge.

Un round garde 40 lignes survivantes, donc il score ses cases contre beaucoup
de préfixes différents — un seul peut tenir sur une carte. Pour nous, c'est la
lignée qui a *réellement* été jouée qui est suivie (les préfixes correspondent
exactement aux premiers coups de la décision) ; pour l'adversaire, dont le coup
n'est jamais imprimé, c'est la meilleure ligne à chaque profondeur.

```sh
make replay LOG=.colosseum/logs/firstenv/run-*/game_*_p0.events.jsonl
make viewer     # puis http://localhost:8000
```

### Commandes du Makefile

| commande | effet |
|---|---|
| `make` / `make play` | compile `main.cpp` seul → `a.out` (le binaire de compétition) |
| `make check` | vérifie que `main.cpp` compile sans l'outil — le garde-fou de la contrainte |
| `make debug` | construit `tools/btk-debug`, uniquement si `main.cpp` ou `debug_tool.cpp` ont changé |
| `make replay LOG=<jsonl>` | construit l'outil si besoin, vide les anciens dumps, puis rejoue la partie |
| `make viewer` | sert l'interface sur `http://localhost:8000` |
| `make clean` | supprime `a.out` et `tools/btk-debug` |

Variables : `LOG` (obligatoire pour `replay`), `TURNS` (défaut 100, plafonné à
la longueur de la partie), `DUMPS` (défaut `tools/dumps`), `PORT` (défaut 8000).

```sh
make replay LOG=<jsonl> TURNS=30     # s'arrêter au tour 30
make viewer PORT=8080                # si le port est déjà pris
```

`replay` efface `tools/dumps/turn_*.json` avant de rejouer : sans ça, une
partie plus courte laisserait derrière elle la fin de la précédente, et le
viewer listerait ces tours comme s'ils appartenaient à la nouvelle.

Sous WSL, un navigateur Windows n'atteint pas le `localhost` de la distro :
`serve.py` affiche au démarrage la seconde adresse (`http://172.x.x.x:8000/`)
qui, elle, fonctionne depuis Chrome. Le terminal intégré de VSCode redirige le
port tout seul, d'où `localhost` qui y marche.

### Images de tuiles (optionnel)

Déposer des PNG carrés dans `tools/tiles/` remplace les cases de couleur :

| fichier | remplace |
|---|---|
| `plains.png` `river.png` `mountain.png` | le terrain |
| `town.png` | les villes (l'id reste écrit par-dessus) |
| `inked.png` | les régions encrées |
| `rail_me.png` `rail_foe.png` `rail_neutral.png` | les pastilles de rail |

Tout est facultatif et indépendant : un fichier manquant retombe sur sa
couleur, donc un jeu partiel fonctionne. Les images n'ont pas besoin d'être
à la même taille — chacune est redimensionnée à la case (30 px). La couleur
du terrain est peinte dessous, donc une image à fond transparent se pose sur
sa teinte plutôt que sur du vide. Dès qu'une image est présente, la heatmap
passe en translucide (0.65) pour la laisser voir, et les chiffres prennent un
contour noir pour rester lisibles sur n'importe quel fond.

`tools/tiles/` est dans `.gitignore` — retirer la ligne pour versionner tes
images.

### Fichiers

- `tools/debug_tool.cpp` — `#include "../main.cpp"`, donc c'est le vrai moteur
  qui est observé, jamais une réimplémentation.
- `tools/serve.py` — sert le viewer et les dumps (stdlib seule).
- `tools/viewer.html` — Canvas : terrain, régions, rails, villes, encre, et la
  heatmap des candidats. La case jouée est entourée ; le panneau latéral
  compare le gain maximum au rail effectivement posé.

Mode live : `./tools/btk-debug live tools/dumps` se comporte comme le bot
(stdin/stdout) et dépose un dump par tour à côté. Utilisable directement comme
bot dans `colosseum.toml`, avec le bouton *Live* du viewer pour suivre.

L'outil garde le budget de `main.cpp` (30 ms, 900 au premier tour) : les
valeurs affichées sont celles que le bot jouera vraiment sur CodinGame.

Un replay reproduit le match de près, mais pas à l'identique, et la raison
tient au bot lui-même : **la recherche s'arrête sur l'horloge, pas sur un
nombre d'itérations, donc elle n'est pas déterministe**. Rejouer cinq fois le
même tour avec le même binaire sur la même entrée donne des sorties
différentes — le beam tronque à un point qui suit les micro-variations de
charge de la machine.

Mesuré sur un log v2.2 de 25 tours : 17 tours rejouent la sortie exacte, 3
posent les mêmes cases dans un autre ordre (sans conséquence pour l'arbitre),
5 diffèrent. Sur le premier rail — celui que la heatmap explique — 22 tours
sur 25 coïncident : la divergence porte presque toujours sur le 2ᵉ ou 3ᵉ rail.

Conséquence pratique : un dump décrit fidèlement *une* exécution de la
recherche à 30 ms, ce qui est bien ce qu'on veut inspecter, mais deux dumps du
même tour peuvent différer. Les valeurs de la heatmap, elles, sont stables :
`extendManhattanGap` ne dépend pas du temps.

## Explanations

## Versions

### v2.8

Reduce main beam width and nested beam on future depths

### v2.7

Replace whish data strcuture by a uint64 bit mask

### v2.6

Sort indexes and corresponding evaluation instead of full beam nodes

### v2.5

Split Map class into mutable + shared-immutable classes, reducing a lot the beam node structure manipulation/copy

### v2.4

Reduce structure byte size

### v2.3

Try nested beam search heuristic improvments

### v2.2

Infinite 'WAIT' turns bug resolved by finding the best non empty action when first beam depth is broken

Last moment in arena: -
First moment in arena: 676/1320 overall & Bronze league

### v2.1

Reduce time budget from 45ms to 30ms : No remaining timeouts
But many games are lost because of infinite WAIT action trhown each turn...

Last moment in arena: 708/1316 overall & Bronze league
First moment in arena: 100/400 Bronze

### v2.0

Nested beam searches: an outer one plans turns ahead, and for each of its
nodes an inner one decides that turn's rails one cell at a time. Both are
interruptible, playing the best line found when the turn budget runs out.

- Rail placement: every affordable cell touching the network, scored by how
    much it shortens the remaining wishes.
- Turn scoring (extendGap): per wish, the terrain distance from a newly
    laid cell to whichever of its two towns is farther.
- State scoring (evaluate): income difference per turn, minus GAP_PENALTY
    per cell of true remaining gap, from one multi-source flood fill
    (openGapTotal) that reads off where two components' floods meet.
- Disrupt choice: the region where the opponent owns the most connection
    rails more than we do. Four disrupts ink a region and erase its rails.

Last moment in arena: 230/450 Bronze
First moment in arena: 191/558 Bronze

### v1.0

- Find shortest distance amongs desired connections to build
- Skip connection if active
- Pick which region to disrupt: one with enemy rails, not yet inked,
    not containing one of our/their towns (can't disrupt those),
    preferring the one closest to being inked / with the most rails

Last moment in arena: 580/115 Bronze
First moment in arena: 191/558 Bronze

### v0.2

Last moment in arena: 320/558 Bronze
First moment in arena: -
