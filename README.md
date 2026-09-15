# codingame_back_track_king

CodinGame Summer Challenge 2026

## Chosen algorithm

### Greedy value map (v3.0)

Pas de recherche du tout. Chaque tour, le plateau est noté une fois, case par
case, et les rails vont sur les meilleures cases que la peinture permet. Un
tour coûte ~100 µs au lieu des 30 ms du beam.

**Au début de la partie** (`Planner::init`)

- Un tableau plat `baseValue[cell]` : `(W+H)/4` si la région de la case
  contient une ville, 0 sinon. Une région à ville ne peut jamais être encrée,
  donc un rail posé là n'est jamais effacé. La valeur est calibrée au quart
  d'une récompense de chemin : assez pour départager deux cases d'un même
  chemin, pas assez pour en battre un.
- Les wishes sont résolus une fois en paires de coordonnées, dédupliqués
  (l'arbitre annonce chaque liaison depuis ses deux villes).
- `cellSlot[cell]` : la région de chaque case, aplatie en `int16` — les
  balayages par région n'ont ainsi jamais à repasser par `regionId`.

**Chaque tour** (`Planner::plan`)

1. `value = baseValue` (une copie mémoire, même taille, pas de réallocation).
2. Pour chaque wish, A* sur le terrain (encre infranchissable) entre les deux
   villes, *active ou non*. Chaque case du chemin reçoit `W+H-coût`, donc les
   liaisons courtes — les seules qu'un tour peut finir — pèsent le plus. Le
   chemin n'est jamais stocké : on remonte la chaîne de parents depuis la
   destination en ajoutant au passage.
3. **DISRUPT** : un balayage linéaire somme, par région, `+value` sous un rail
   adverse et `-value` sous un des miens. La meilleure région non encrée et
   sans ville est la cible, ou aucune si le meilleur score est ≤ 0.
4. `value *= (INK_SCALE - instabilité)` par région. La division par
   `INK_SCALE` de la formule `(5 - inkLevel) / 5` n'est jamais faite : elle est
   la même pour toutes les cases, donc la supprimer laisse le classement
   intact et garde les valeurs exactes en entier.
5. **Rails** : jusqu'à trois tours de boucle, chacun un balayage linéaire qui
   garde la meilleure case jouable et abordable. Le deuxième rail est donc
   choisi en sachant le premier.
6. **Diffusion** (`diffuseValues`), seulement quand le balayage ne trouve plus
   rien à poser alors qu'il reste de la peinture : chaque case encore à zéro
   prend **la moitié de chaque voisine valuée**, sommée — une case entre deux
   voisines valuées reçoit donc les deux moitiés. La passe lit un instantané
   et écrit dans `value`, donc une case remplie par la passe ne nourrit pas la
   suivante *dans* la même passe : un appel = un anneau. On rappelle jusqu'à
   trouver une case, et la division par deux tue le front dès qu'il passe sous
   2, donc la boucle se termine seule. L'encre et l'infranchissable ne
   conduisent rien : aucun rail ne peut y vivre.

Le choix des rails se fait sur une clé 64 bits construite en place
(`value << 23 | touche-le-réseau << 22 | (3-coût) << 20 | (N-1-idx)`), donc la
comparaison du balayage est un seul entier.

**L'égalité est le cas normal**, pas l'exception : un chemin récompense toutes
ses cases à l'identique. Les départages, dans l'ordre :

1. **adjacent au réseau** (rail ou ville voisine, y compris un rail posé plus
   tôt dans le même tour). C'est ce qui transforme une ligne de cases égales
   en une ligne *construite* : sans lui le remplissage suivrait l'ordre de
   balayage et fragmenterait le chemin.
2. **terrain le moins cher** — à valeur égale, une plaine laisse deux rails de
   plus dans le tour qu'une montagne.
3. **ordre de balayage**, pour rester déterministe.

La diffusion est ce qui remplit la fin de partie. Les plus courts chemins sont
bâtis bien avant la fin du temps : sans elle, sur une partie gagnée 5116-1575,
**65 tours sur 100 étaient des `WAIT`** et le bot posait 86 rails. Avec, zéro
`WAIT` et 190 rails, pour ~36 µs par tour.

### Critiques de l'algorithme

- **Le classement ignore le prix.** Une montagne à 100 bat trois plaines à 90,
  alors que les trois plaines valent 270 pour la même peinture. Trier sur
  `valeur / coût` (ou faire un vrai sac à dos sur 3 points de peinture, ce qui
  est trivial à cette taille) est le changement le plus rentable à essayer.
- **Les chemins A* ignorent les rails déjà posés.** Une liaison déjà active
  par un autre tracé continue d'attirer des rails sur son chemin *théorique*,
  qui ne sert plus à rien. Donner un coût 0 aux cases déjà railées ferait
  fondre la récompense sur le travail restant — et ferait monter les liaisons
  presque finies en tête, ce qui est exactement le bon réflexe.
- **Rien ne regarde l'adversaire** en dehors du DISRUPT. Le beam simulait son
  tour ; ici il peut prendre la case visée sans qu'on le voie venir.
- **Un seul chemin par wish.** Deux tracés de même longueur existent souvent ;
  n'en récompenser qu'un fixe arbitrairement le tracé. Récompenser toutes les
  cases sur *un* plus court chemin (un double A* depuis chaque ville, garder
  `dA + dB == distance`) donnerait un couloir plutôt qu'une ligne.
- **Le DISRUPT ne tient pas compte de l'instabilité déjà accumulée.** Une
  région à 3/4 est à un coup de l'encre, une région à 0/4 en demande quatre :
  à score égal la première vaut bien plus.

## Debug viewer

`tools/` contient une interface qui affiche la map et **la valeur que
l'algorithme a donnée à chaque case** ce tour-là — exactement le nombre sur
lequel le coup a été lu. Un tour = une seule carte (le planner note tout le
plateau d'un coup), donc plus de rounds ni de préfixes à sélectionner.

`main.cpp` reste compilable et jouable seul — c'est `make check` qui le
garantit, et le binaire de compétition est inchangé au bit près (les hooks
sont des macros vides hors `DEBUG_TOOL`).

Sur le plateau :

- **heatmap + chiffre** dans chaque case jouable : la valeur finale, après la
  remise d'encre. Les zéros sont laissés vides, ils sont la majorité.
- **anneaux blancs numérotés** sur les rails posés ce tour, dans l'ordre où le
  planner les a choisis.
- **contour orange** autour de la région DISRUPT.
- le panneau latéral classe les régions encrables par score de disrupt, et le
  survol d'une case donne sa valeur, sa région et son score.

Contrairement au beam, **le planner est déterministe** : il ne s'arrête pas
sur l'horloge. Rejouer le même log avec le même binaire donne exactement la
même sortie, et un replay qui diffère du log veut dire que le log a été écrit
par une autre version du bot.

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
  carte des valeurs.

Mode live : `./tools/btk-debug live tools/dumps` se comporte comme le bot
(stdin/stdout) et dépose un dump par tour à côté. Utilisable directement comme
bot dans `colosseum.toml`, avec le bouton *Live* du viewer pour suivre.

## Idées à essayer

- Sac à dos sur les 3 points de peinture plutôt qu'un greedy par valeur brute
  (voir les critiques ci-dessus).
- Couloir de plus courts chemins plutôt qu'un seul tracé, par double A*.
- A* à coût 0 sur les rails existants, pour que la récompense mesure le
  travail restant et non la distance théorique.
- Pondérer le score de DISRUPT par l'instabilité déjà accumulée.
- Simuler le tour de l'adversaire avec le même planner (il est assez rapide
  pour tourner deux fois) et éviter les cases qu'il va prendre.

## Historique — BEAM search (v2.x)

Le code de ces versions est dans `backtrackking_v2.*.cpp`.

Deux beam searches imbriqués : l'extérieur planifiait les tours à venir, et
pour chacun de ses nœuds l'intérieur décidait les rails du tour une case à la
fois. Les deux étaient interruptibles et jouaient la meilleure ligne trouvée
quand le budget de 30 ms tombait.

- **Création des placements** : toute case abordable touchant le réseau, notée
  par ce qu'elle raccourcit des wishes restants (`extendManhattanGap`).
- **Heuristique d'état** (`evaluate`) : revenu par tour, moins `GAP_PENALTY`
  par case de gap restant, mesuré par un flood fill multi-source
  (`openGapTotal`) qui lit où deux composantes se rencontrent.
- **DISRUPT** : la région où l'adversaire possède le plus de rails de
  connexion de plus que moi.

Ce qui l'a fait abandonner : `openGapTotal` prenait la moitié du temps total
et refaisait un flood fill complet par nœud alors que des nœuds consécutifs ne
diffèrent que de ~3 rails ; et la profondeur atteinte restait faible.

### Idées de l'époque encore valables

- Une lookup table de `path` entre 2 cases (distance A* + liste des régions
  traversées), plus un index inverse région → chemins, pour ne recalculer que
  les chemins invalidés quand une région est encrée. `PathTable` faisait ça en
  v2.x ; le planner v3 recalcule tout, ce qui coûte moins cher que le cache à
  cette taille de plateau.
- Pour le pruning des actions : prioriser les cases sur plusieurs plus courts
  chemins, celles qui ne peuvent pas être encrées, écarter celles qui vont
  l'être. C'est devenu la carte de valeurs de v3.

### GA pour construire un graph pondéré — ne fonctionne pas

Trouver la longueur des chemins les plus courts entre chaque ville, créer un
graph pondéré, faire un GA qui coupe et crée des liaisons pour minimiser la
distance totale.

- Les chemins doivent pouvoir être liés n'importe où, pas que sur des villes.
- Construire un graph global ne rapporte pas beaucoup de points par rapport à
  faire plein de liaisons rapidement. Trop lent.

## Versions

### v3.1

Diffusion des valeurs quand le tour ne peut plus rien poser : les cases à zéro
prennent la moitié de chaque voisine valuée. 65 tours `WAIT` sur 100 → 0, et
86 rails posés → 190, pour ~36 µs par tour.

Battle locale contre v3.0 (119 parties valides, positions échangées) :
**109 V – 0 D – 10 nulles**. v3.0 ne gagne aucune partie.

### v3.0

Greedy sans recherche : une carte de valeurs par tour, les rails sur les
meilleures cases abordables. ~100 µs par tour au lieu de 30 ms.

Battle locale contre v2.8 (121 parties, positions échangées) : **92.9 % — 7.1 %**.

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
