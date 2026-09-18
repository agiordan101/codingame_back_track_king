# codingame_back_track_king

CodinGame Summer Challenge 2026

## Chosen algorithm

### Beam search (v5)

Le bot ne juge plus le tour à un coup d'avance : il déroule **plusieurs tours**
et joue le premier coup de la meilleure ligne trouvée. La machinerie de v4 est
gardée telle quelle — elle sert maintenant d'élagage sous le beam.

#### Un niveau de beam

Un état, c'est le plateau quelques tours plus loin. Pour chaque état gardé :

1. Noter le plateau : c'est la carte des valeurs de v4, aux trois couches
   inchangées.
2. Retenir les **24 meilleures cases**, en former **toutes** les combinaisons
   que 3 peintures payent, et n'en jouer que les `COMBO_PRUNING_WIDTH`
   meilleures. Le tour qu'on est en train de jouer est exempté : à la racine
   on les joue toutes.
3. Croiser avec les **3 meilleures régions à encrer**, plus le plateau sans
   encrage.
4. Poser les rails sur le plateau, encrer, **recalculer le revenu des deux
   joueurs**, noter l'état obtenu.
5. Garder les `BEAM_WIDTH` meilleurs états de tout le niveau, doublons écartés.

Et recommencer, jusqu'à épuisement des 30 ms. Le coup joué est le premier tour
de la meilleure ligne du dernier niveau atteint.

#### Ce que le beam achète

**Un rail qui ne paye que le tour suivant.** v4 notait le plateau un tour plus
loin : une montagne qui termine une liaison en deux tours ressemblait à trois
peintures jetées. Le revenu étant compté à chaque tour de la ligne, une liaison
finie tôt rapporte autant de fois qu'il reste de tours — « finir vite » sort
tout seul de l'heuristique, sans terme dédié.

**Un DISRUPT honnête.** v4 notait chaque encrage comme s'il détruisait la
région sur-le-champ ; sur plusieurs tours ce mensonge devient intenable. Ici un
DISRUPT monte l'instabilité de 1 et n'encre qu'au seuil. Comme les trois
premiers coups ne changent rien au plateau, ils ne changeraient rien à la note
non plus : chaque coup touche donc **un quart de ce que vaut la région**, et ce
crédit est **remboursé** au coup qui encre, où le revenu prend le relais pour de
bon. C'est l'idée du « DISRUPT différé » notée en v4.5.

#### Rien n'est copié

Le planner possède **un seul plateau de travail** — propriétaire, réseau, encre,
instabilité, un octet par case — et un coup s'y **applique et se défait**. Une
ligne se rejoue en empilant ses coups, un enfant s'évalue en posant ses rails et
en les retirant. Le classement d'un niveau **trie des entiers** `(note, indice)`
empaquetés : un état est écrit une fois, quand il est gardé, et ne bouge plus.

Deux lignes qui posent les mêmes rails dans un autre ordre arrivent sur le même
plateau : un hash Zobrist des rails et des instabilités les fait fusionner.

#### Ce qui coûte, et ce qui a été fait pour

Tout le temps part dans le **recalcul du revenu** : chaque wish est re-résolu à
la main sur le plateau imaginé. Deux choses le contiennent :

- **Les états qui partagent un plateau ne le payent qu'une fois.** Un DISRUPT
  qui n'encre pas ne change rien au plateau : ses enfants sont ceux du plateau
  sans encrage, à un crédit près, et **aucun wish n'est résolu pour eux**.
  Mieux : entre deux encrages sans effet, le mieux classé gagne toujours — même
  plateau, même revenu, crédit plus gros — donc un seul enfant est écrit, et la
  largeur du beam sert à des lignes vraiment différentes. Seuls les encrages qui
  *atteignent le seuil* rejouent les combinaisons, sur le plateau amputé.
- **Un wish n'est re-résolu que s'il peut avoir bougé.** Le plateau de l'état
  est balayé depuis *les deux* villes de chaque wish, ce qui donne la distance
  de toute case à chacune d'elles. Un chemin passant par les nouveaux rails
  entre par l'un et sort par un autre, et ce qu'il fait entre les deux coûte au
  moins la distance de grille : `d_src[a] + |a−b| + d_dst[b]`, pris au minimum
  sur les paires, **minore** donc tout chemin neuf. Plus long que le chemin
  actuel ⇒ le wish n'a pas pu changer. Vérifié sur 3 parties entières : zéro
  raté.

Mesuré : **10,5 → 2,6 résolutions par plateau**, 4 000 → 6 600 plateaux notés
par tour, profondeur 9 → 14.

**Pourquoi le filtre doit compter les égalités.** L'arbitre départage les
chemins de même longueur en NESW depuis la ville demandeuse. Un rail neuf qui
*égale* la longueur actuelle peut donc voler le chemin — et les points qui vont
avec. D'où `≤` et non `<`.

**Pourquoi la borne prend des paires.** Deux rails neufs non adjacents peuvent
se rejoindre **par de l'ancien réseau** : le chemin entre alors par le premier
et sort par le second. Ne regarder que les cases une par une rate ce cas — c'est
un vrai bug, mesuré à 1 392 ratés sur une partie avant correction.

#### La carte des valeurs ne se rebâtit presque jamais

Les couches 1 et 2 ne lisent que le terrain et l'encre, **jamais les rails** :
la carte ne dépend donc que de l'ensemble des régions encrées, et tous les états
qui n'ont rien encré de neuf partagent la même. Un masque de bits des régions
encrées sert de clé de cache. Mesuré : **2 reconstructions par tour**, pour une
centaine d'états développés.

La couche 3 (la remise d'encre) est appliquée au moment de classer les cases, pas
dans la carte : le beam fait monter l'instabilité au fil des tours, et la carte
en dessous reste bonne pour toute la ligne.

#### Ce qui compose la note d'une case

| # | terme | effet | condition |
|---|---|---|---|
| 1 | **bonus région-à-ville** | `+ (W+H)/4` | la région contient une ville — elle ne peut jamais être encrée, le rail n'y sera jamais effacé |
| 2 | **récompense de chemin** | `+ max(1, W+H − coût du chemin)`, **une fois par chemin** | la case est sur le plus court chemin d'un wish ; cumulatif si plusieurs s'y croisent |
| 3 | **remise d'encre** | `× (5 − instabilité)` | par région, instabilité plafonnée à 4 : une région proche de l'encre porte des rails bientôt effacés |
| 4 | **diffusion** | `+` moitié de chaque voisine valuée, sommée | uniquement sur les cases restées à 0, et seulement s'il reste de la peinture sans case à acheter |

Le terme 2 est le moteur : une liaison courte vaut plus qu'une longue, parce
que c'est celle qu'un tour peut finir. « Courte » se compte **en peinture, pas
en cases** : l'arbitre note bien une liaison au nombre de cases, mais ce A*
choisit où dépenser nos 3 points du tour, et une montagne en coûte vraiment 3.
Mesurer l'inverse a coûté 20 points de winrate (v3.11, abandonnée).

#### Comment un état est noté

`(revenu encaissé − revenu adverse) × 2²⁴ + valeur des cases achetées + crédits
de DISRUPT`

Le revenu est celui de **toute la ligne** : chaque tour simulé rapporte à chacun
1 point par rail qu'il possède sur le chemin actif de chaque wish. Les scores de
départ ne sont pas comptés — ils sont les mêmes pour tous les états d'un niveau.

Un point de revenu vaut plus que n'importe quelle somme de notes : gagner du
score passe toujours avant accumuler du bon terrain. Le poids est passé de
10 000 à 2²⁴ parce que le beam somme les cases de **toute une ligne**, pas d'un
seul tour.

#### Ce qui départage deux cases de même note

L'égalité est le cas normal : un chemin récompense toutes ses cases à
l'identique. Dans l'ordre : la note, puis l'adjacence au réseau déjà posé,
puis le terrain le moins cher, puis l'ordre de balayage pour rester
déterministe.

#### Trois règles apprises à la dure

**Ne jamais diffuser la carte que les rails classent.** Essayé en v3.2 : le
halo remonte une montagne collée au couloir au-dessus d'une plaine plus loin
sur ce même couloir, et **58 tours sur 100 passaient les 3 peintures dans une
seule montagne**. 4 V – 116 D contre v3.1. D'où la copie pour le DISRUPT.

**Le sens du parcours compte.** L'A* départage NESW en marchant depuis la
source, et ne réécrit un parent que sur une amélioration stricte : partir de B
au lieu de A donne un autre couloir sur **3,8 % des paires**. D'où
l'orientation de l'arbitre conservée telle quelle.

**La diffusion remplit la fin de partie.** Les plus courts chemins sont bâtis
bien avant la fin : sans elle, **65 tours sur 100 étaient des `WAIT`** et le
bot posait 86 rails ; avec, zéro `WAIT` et 190 rails.

#### Ne pas élaguer les cases, élaguer les combinaisons

Le réflexe — « tirer les combinaisons des 6 meilleures cases au lieu de 24 » —
est **le pire réglage mesuré** : à profondeur 1, il perd 45 points de winrate
d'un coup. Les cases finalement achetées ne tiennent dans les 6 premières que
77 % du temps, et ce qui est jeté là ne revient jamais.

Ce qu'il faut couper, c'est la **liste des combinaisons**, pas le vivier de
cases : toutes les combinaisons des 24 cases sont formées (quelques milliers
d'additions), triées par somme de notes, et seules les meilleures sont jouées.
Et **jamais à la racine** : la somme des notes ne prédit pas le revenu, donc
couper le classement du tour qu'on joue vraiment coûte encore 16 points.

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

**Le planner s'arrête sur l'horloge**, donc un rejeu n'est plus reproductible
au bit près : une machine plus chargée voit un niveau de moins et peut jouer
un autre coup. Un replay qui diffère du log ne prouve donc plus que le log
vient d'une autre version — c'est le prix du beam, que v3 et v4 n'avaient pas.

```sh
make replay LOG=.colosseum/logs/firstenv/run-*/game_*_p0.events.jsonl
make viewer     # puis http://localhost:8000
```

### Observer un bot précis

Jouer la partie en la logguant, rejouer le log, servir le viewer :

```sh
cg-colosseum battle v4.0 v3.10 -n 1 --seed 42 -l .colosseum/logs/v3_comp/mine
make replay LOG=.colosseum/logs/v3_comp/mine/game_*_p0.events.jsonl
make viewer
```

Chaque partie écrit **deux logs, un par joueur** : `_p0` est le premier bot
cité, `_p1` le second. C'est ce suffixe qui choisit le bot observé, pas
l'ordre des arguments de `replay`.

Attention : le log ne contient que ce que l'arbitre a envoyé à ce joueur, et
`make replay` le rejoue dans le `main.cpp` **compilé maintenant**. Pour voir
jouer une ancienne version il faut donc aussi restaurer son `main.cpp` ;
sinon on regarde la version courante rejouer les situations que l'ancienne a
rencontrées — utile pour comparer deux versions sur les mêmes plateaux, mais
c'est un autre usage.

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

- **Simuler l'adversaire.** Au-delà de deux ou trois tours, la ligne suppose un
  adversaire figé : il ne pose rien, ne prend aucune de nos cases, n'encre rien.
  v4.6 a essayé de prédire son coup (même carte de valeurs, même classement) et
  c'était moins bon — mais c'était sans beam. À retenter au-dessus du beam, ne
  serait-ce qu'aux deux premiers niveaux.
- **L'ordre d'évaluation des combinaisons, aux niveaux profonds.** Elles sont
  triées par somme de notes, et la note ne prédit pas le revenu — c'est mesuré :
  couper ce classement à la racine coûte 16 points. Aux niveaux profonds on le
  coupe quand même, faute de budget. Trier plutôt par « nombre de cases du
  groupe sur un chemin actif » ferait remonter les coups payants.
- **Les balayages de distance sont devenus le plancher du coût** : 2 par wish et
  par état, une centaine d'états, ~10 000 balayages par tour. Le plateau d'un
  enfant ne diffère de celui de son père que de 3 rails ; une mise à jour
  incrémentale des distances les remplacerait presque tous.
- **Répartir le budget par profondeur.** La racine joue toutes ses
  combinaisons, les niveaux suivants en jouent `COMBO_PRUNING_WIDTH`. Entre les
  deux il n'y a rien : un dégradé (large en haut, étroit en bas) est sans doute
  meilleur que la marche d'escalier actuelle.
- diffusion : Ajouter 4 moitiés sur un case donne un score plus grand que les cases elles mêmes. Il faut ajouter 1/4 ? Est ce qu'on veut que ces cases puissent être meilleur que d'des principales a* ?
- Moins punir les cases inked

### Idées de l'époque encore valables

- Une lookup table de `path` entre 2 cases (distance A* + liste des régions
  traversées), plus un index inverse région → chemins, pour ne recalculer que
  les chemins invalidés quand une région est encrée. `PathTable` faisait ça en
  v2.x ; le planner v3 recalcule tout, ce qui coûte moins cher que le cache à
  cette taille de plateau.

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

**Mais le nul n'est un risque que si les deux versions se ressemblent.** Le
diagnostic est le **nombre de nulles**, pas le principe du duel : v4.0 contre
v3.10 en a rendu **1 sur 600**, là où deux quasi-jumeaux en rendaient 636 sur
930. Quand les deux bots jouent vraiment différemment, le duel direct est
valide — et c'est lui qui renseigne.

**v3.1 est saturé, ne plus s'en servir pour départager.** Les trois versions
ci-dessous sont indiscernables face à lui :

| candidat | contre v3.1 |
|---|---|
| v3.4 | 96,7 % (94,9–97,8) |
| v3.10 | 96,8 % (95,1–97,9) |
| v4.0 | 96,7 % (94,9–97,8) |

Or v4.0 bat v3.10 à **78,7 %** en direct. À ~97 % il ne reste qu'une vingtaine
de parties pour discriminer : l'intervalle de confiance est plus large que
l'écart qu'on cherche. Mesurer **contre la version en place**, et n'utiliser un
tiers que si les nulles montent.

```sh
cg-colosseum compare <candidat>  v3.1 -n 300 -s -t 8 --no-log
cg-colosseum compare <reference> v3.1 -n 300 -s -t 8 --no-log
```

Signal d'alarme : un taux de nulles élevé, ou un verdict extrême (0 %, 100 %)
sans nulles du côté opposé, veut dire que la mesure est cassée, pas que le bot
l'est.

## Versions

### v5.0

**Beam search sur les tours**, posé sur l'élagage de v4 : le coup joué est le
premier tour de la meilleure ligne, et non plus la meilleure ligne d'un tour.

**66,7 % contre v4.5** (200 V – 98 D – 2 N, 300 parties, `-t 4`).

Réglages : `BEAM_WIDTH=4`, `COMBO_PRUNING_WIDTH=64`, `MAX_BEAM_DEPTH=32`,
vivier de cases inchangé à 24. Profondeur atteinte ~20 tours, 8 400 plateaux
notés par tour (v4.5 : 1 tour, 4 700 plateaux).

Deux des trois réglages étaient des pièges, et c'est là qu'est passé l'essentiel
du travail :

| réglage | contre v4.5 (80 parties) |
|---|---|
| vivier réduit à 6 cases, profondeur 1 | 27,5 % |
| vivier 24, 32 combinaisons jouées, profondeur 1 | 41,2 % |
| vivier 24, toutes combinaisons, profondeur 1 | ~57 % |
| vivier 24, racine exhaustive, beam `w2 c64 d8` | 66,7 % |
| vivier 24, racine exhaustive, beam `w4 c64 d32` | 66,7 % (66,7 % sur 300) |
| beam `w8 c64 d32` | 46,9 % |

**Le vivier de cases ne doit pas être élagué** (−45 points) et **la racine ne
doit pas voir son classement de combinaisons coupé** (−16 points). Le beam
rapporte une dizaine de points une fois ces deux erreurs évitées, et le budget
va aux tours d'une ligne plutôt qu'au nombre de lignes : 2 et 4 se valent, 8 et
16 décrochent.

**Le filtre de revenu** — balayage des deux villes de chaque wish, puis borne
par paires sur les nouveaux rails — fait tomber les résolutions de 10,5 à 2,6
par plateau : +65 % de plateaux notés, profondeur 9 → 14 à réglages égaux.
Vérifié exhaustivement sur 3 parties : jamais un changement de revenu raté.

Ce que v5 sait faire que v4 ne savait pas : acheter un rail qui ne paye qu'au
tour suivant, et monter l'instabilité d'une région sur plusieurs tours pour
l'encrer au moment où ça coupe le plus.

Ce qu'elle suppose : **l'adversaire est figé** sur toute la ligne. C'est faux,
et c'est la première chose à attaquer.

Attention : le planner n'est plus déterministe (il s'arrête sur l'horloge), donc
deux mesures du même binaire diffèrent, et une machine chargée fait des
timeouts. Mesurer avec `-t 4`, pas `-t 8`.

Last moment in arena: -

First moment in arena: -

### v4.5

- **Le revenu est recalculé, plus lu.** : 1 point par rail possédé sur le chemin au lieu de 1 par wish
- **Seuls les wishes atteignables par les 3 rails sont recalculés** — 12 ms
  → 426 µs. Le filtre doit inclure la frontière du parcours, sinon il est faux.
- **Horloge lue à chaque état** (avant : 1 sur 64)

Les cases choisies tiennent dans les N meilleures	| Fréquence
3	55,7 %
4	69,6 %
6	77,7 %

Last moment in arena: -

First moment in arena: 264/1439 overall & Silver league

### v4.0

Remplace le greedy par une **recherche sur combinaisons**. Toutes les
combinaisons de rails de coût ≤ 3 tirées des 24 meilleures cases, contre le
plateau sans DISRUPT et les 4 meilleures régions ; heuristique
`différence de score × 10000 + somme des valeurs`.

**78,7 % contre v3.10** (75,2–81,8 %, 600 games) — premier gain net depuis
v3.4. Le greedy posait 229 rails par partie, la recherche 246, et 38 tours sur
100 changent de jeu de rails.

Le veto de v3.10 est supprimé (redondant avec la différence de score). Une
règle a dû être ajoutée : prendre un DISRUPT à égalité de plateau, sans quoi
l'heuristique un-tour n'en joue aucun.

Last moment in arena: 343/1439 overall & Silver league

First moment in arena: 333/1439 overall & Silver league

### v3.11 — abandonnée (significativement pire)

Le A* ne pondérait plus les cases par leur coût de peinture : chaque case
passable coûtait 1, au motif que l'arbitre note une liaison au **nombre de
cases**. Divergence bien réelle (3775 paires hors jeu : 84,5 % des trajets
changent, 20,3 % étaient plus longs que nécessaire en cases, 2,6 cases de
moyenne), mais **29,8 % de winrate contre v3.10** (26,3–33,6 %, 600 games).

Leçon : ce A* ne prédit pas le score de l'adversaire, il décide où poser notre
peinture. Un chemin court en cases qui franchit deux montagnes coûte deux tours
pleins — le coût terrain était donc le bon critère pour la question réellement
tranchée ici : quel tracé on a les moyens de finir.

### v3.10

Vérifie que les région que l'on veut DISRUPT ne participent pas à des chemins qui me rapportent plus de points qu'à l'adversaire

Last moment in arena: 412/1468 overall & Silver league

First moment in arena: 315/1439 overall & Silver league

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

![alt text](image.png)

L'A* part de la ville demandeuse, et non de celle au plus petit id. Le tri par
id dans `init` annulait le départage NESW sur ~3,8 % des paires ; il ne
dédupliquait rien, aucun wish n'étant déclaré deux fois.

Last moment in arena: 354/1435 overall & Silver league

First moment in arena: 801/1435 -> 354/1435 overall & Silver league

Bronze submit: 99 WIN / 2 LOSES

![alt text](image-2.png)

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

Last moment in arena: 810/1435 overall & Bronze league

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
