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
2. Pour chaque wish, A* sur le terrain (encre infranchissable) **de la ville
   demandeuse vers la ville souhaitée**, *active ou non*. Chaque case du chemin reçoit `W+H-coût`, donc les
   liaisons courtes — les seules qu'un tour peut finir — pèsent le plus. Le
   chemin n'est jamais stocké : on remonte la chaîne de parents depuis la
   destination en ajoutant au passage.
3. **DISRUPT** : un balayage linéaire somme, par région, `+value` sous un rail
   adverse et `-value` sous un des miens. La meilleure région non encrée et
   sans ville est la cible, ou aucune si le meilleur score est ≤ 0.
   Le score se lit sur **une copie diffusée** (`disruptValue`), jamais sur
   `value`. Le couloir fait une case de large et est arbitraire parmi les
   chemins de même longueur : l'adversaire construit *à côté*, pas dessus, et
   pèse donc zéro sur la carte brute. Un anneau de diffusion est ce qui fait
   compter ses rails. La copie est ce qui garde ça hors du classement des
   rails — voir plus bas.
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

**Le sens du parcours compte.** L'A* départage ses égalités dans l'ordre
NORTH, EAST, SOUTH, WEST (`DIR_X`/`DIR_Y`), en marchant *depuis la source* :
la relaxation n'écrase un parent que sur une amélioration stricte
(`ng >= gScore[nIdx]` saute), donc la première direction à atteindre une case
au coût optimal la garde. Partir de B au lieu de A donne donc un autre couloir
partout où plusieurs plus courts chemins existent — mesuré à **3,8 % des
paires** sur terrain varié (0 % sur plaine uniforme, où le couloir est
symétrique). D'où l'orientation conservée telle que l'arbitre l'annonce :
`readTowns` émet `(ville demandeuse, ville voulue)`, et `init` ne trie plus la
paire par id. Aucun wish n'est déclaré par ses deux villes — vérifié sur le
plateau : 13 déclarations, 13 paires uniques, 0 mutuelle.

**L'égalité est le cas normal**, pas l'exception : un chemin récompense toutes
ses cases à l'identique. Les départages, dans l'ordre :

1. **adjacent au réseau** (rail ou ville voisine, y compris un rail posé plus
   tôt dans le même tour). C'est ce qui transforme une ligne de cases égales
   en une ligne *construite* : sans lui le remplissage suivrait l'ordre de
   balayage et fragmenterait le chemin.
2. **terrain le moins cher** — à valeur égale, une plaine laisse deux rails de
   plus dans le tour qu'une montagne.
3. **ordre de balayage**, pour rester déterministe.

**Ne jamais diffuser la carte que les rails classent.** Essayé en v3.2 : une
passe inconditionnelle sur `value` juste après `buildValueMap`. Le halo remonte
une montagne collée au couloir au-dessus d'une plaine plus loin sur ce même
couloir, et comme `bestCell` classe sur la valeur brute, **58 tours sur 100
passaient les 3 peintures dans une seule montagne** (tour 5 : une case à 540
pour 3 peintures, là où trois plaines valaient 1190). Résultat : 4 V – 116 D
contre v3.1. La diffusion pour le DISRUPT doit donc se faire sur une copie.

La diffusion est ce qui remplit la fin de partie. Les plus courts chemins sont
bâtis bien avant la fin du temps : sans elle, sur une partie gagnée 5116-1575,
**65 tours sur 100 étaient des `WAIT`** et le bot posait 86 rails. Avec, zéro
`WAIT` et 190 rails, pour ~36 µs par tour.

### Critiques de l'algorithme

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

- Pondérer le score de DISRUPT par l'instabilité déjà accumulée.
- Simuler le tour de l'adversaire avec le même planner (il est assez rapide
  pour tourner deux fois) et éviter les cases qu'il va prendre.

- Use partOfActiveConnections:
  Une chaîne de paires townId séparées par des virgules indiquant que cette case fait partie d’une connexion active entre ces deux villes.
  ex. " 1-2,1-3,4-7 ": la case fait partie du chemin le plus court entre les villes 1 & 2, villes 1 & 3, et villes 4 & 7.
  " x " si cette case ne fait partie d’aucune connexion active.

## Idées pour le prochain algorithm

Lorsqu'on mettra un beam search par dessus, on pourrait faire des depth entre les tours pour choisir quelle région à disrupt parmis les 3 meilleurs.

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

## Protocole de mesure

**Ne jamais comparer deux candidats l'un contre l'autre.** Le planner est
déterministe : deux versions proches visent la même case au même tour, le
moteur marque le rail `NEUTRAL_OWNER`, personne ne le possède, aucune
connexion ne paie. **v3.4 contre lui-même donne 0-0 systématiquement.**

Le duel v3.8 vs v3.4 a ainsi rendu 636 nulles sur 930 et un verdict de 0,0 %,
alors que v3.8 vaut 94,1 % contre un adversaire tiers. Le biais est d'autant
plus fort que les deux versions se ressemblent, donc il frappe exactement les
comparaisons qu'on veut faire.

Le bon protocole : **les deux candidats contre un même adversaire tiers**, et
on compare les deux taux.

```sh
cg-colosseum compare <candidat>  v3.1 -n 300 -s -t 8 --no-log
cg-colosseum compare <reference> v3.1 -n 300 -s -t 8 --no-log
```

Signal d'alarme : un taux de nulles élevé, ou un verdict extrême (0 %, 100 %)
sans nulles du côté opposé, veut dire que la mesure est cassée, pas que le bot
l'est.

## Versions

### v3.10

Vérifie que les région que l'on veut DISRUPT ne participe pas à des chemins qui me rapportent plus de points qu'à l'adversaire

### v3.9 — abandonnée (non concluante)

`partOfActiveConnections` agrégé par case en un simple compteur
(`Map::activeConnCount`, rempli pendant le parsing existant), utilisé
uniquement par `chooseDisrupt` : chaque rail y compte
`valeur * (1 + 2 * traversées)`, des deux côtés de la soustraction — un rail
actif rapporte à son propriétaire chaque tour, le nôtre autant.

Mesuré contre v3.1 : **96,3 %** (94,6–97,6) contre **96,7 %** (94,9–97,8) pour
v3.4. Intervalles quasi superposés, **aucun écart démontré**.

Le signal est pourtant bien réel (vérifié sur log : 14 à 46 traversées par tour
dès le tour 1). L'enseignement est donc sur le levier, pas sur l'implémentation :
**changer la cible du DISRUPT ne déplace pas le résultat**. Le plafond est dans
le placement des rails.

Limite de mesure à retenir : à 96 % contre v3.1, il ne reste qu'une vingtaine de
défaites pour départager deux candidats — le bruit domine. Départager mieux que
v3.4 demanderait un adversaire tiers plus fort, et l'environnement n'en contient
pas (v3.0 est plus faible, v3.5→v3.8 sont des régressions).

### v3.8 — abandonnée

`partOfActiveConnections` exploité par case : une case vide sur un chemin
payant vaut ce qu'elle rapporterait par tour, doublé si elle jouxte un rail
adverse payant. Mesuré contre v3.1 : **94,1 %** contre **96,7 %** pour v3.4.
N'apporte rien avec `INCOME_WEIGHT = 12`, le seul poids essayé.

### v3.7 — abandonnée

Mélange des deux routes : terrain (ligne long terme, poids 3) + réseau (rails
gratuits, poids `1 + 24/restant`). 37,3 % contre v3.4 — mais mesuré avec le
protocole biaisé, donc à reprendre si l'idée est relancée. Défaut constaté :
99 % de la valeur se déposait sur des cases injouables.

### v3.6 — abandonnée

Ne payer que les cases vides du chemin. 1,6 % contre v3.4, chiffre lui aussi
suspect (protocole biaisé). Enseignement retenu : les cases déjà railées
portent la mémoire du tracé d'un tour sur l'autre.

### v3.4

L'A* part de la ville demandeuse, et non de celle au plus petit id. Le tri par
id dans `init` annulait le départage NESW sur ~3,8 % des paires ; il ne
dédupliquait rien, aucun wish n'étant déclaré deux fois.

### v3.3

Diffuse la valeurs des cases seulement pour la selection du DISRUPT.
Battle locale contre v3.1 (120 parties, positions échangées) : **118 V – 2 D**.

### v3.2 — abandonnée

Diffuse la valeurs des cases tout le temps.
Problème : Les cases "évité" comme les montagne ayant 3 voisins héritent de plusieurs cases voisines et biaise les stats.

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
