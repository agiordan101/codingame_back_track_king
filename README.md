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
3. **Jouer le tour de l'adversaire** sur ce même classement (v5.1) : les
   meilleures cases que ses 3 peintures payent, et la région qui efface le
   plus des nôtres pour le moins des siennes.
4. Croiser avec les **3 meilleures régions à encrer**, plus le plateau sans
   encrage.
5. Poser les rails sur le plateau, encrer, **recalculer le revenu des deux
   joueurs**, noter l'état obtenu.
6. Garder les `BEAM_WIDTH` meilleurs états de tout le niveau, doublons écartés.

#### L'adversaire, joué à chaque tour de chaque ligne (v5.1)

**Il lit le plateau avec la même carte que nous.** Elle est symétrique — bâtie
sur les souhaits des villes, sans camp dedans — donc son tour est la tête de
notre propre classement : les meilleures cases dans l'ordre tant que ses trois
peintures tiennent, et pour l'encrage, l'entrée la plus **négative** du score
de disrupt, qui vaut exactement « ses rails moins les miens » vu de l'autre
côté. Sa cible se lit donc dans le tableau déjà calculé, sans un octet de plus.

**Un tour se résout comme l'arbitre le résout** : ses rails, les nôtres à côté,
puis les encrages. Une case peinte par les deux le même tour n'appartient à
personne — elle porte le chemin et ne rapporte à aucun des deux. Les deux
joueurs visant la même carte, le cas est fréquent, et c'est ce qui rend le
contest payant : lui refuser une case vaut ce qu'elle lui aurait rapporté.

**La valeur des cases est donc une différence**, comme le revenu au-dessus
d'elle : `nos cases − les siennes restées à lui`. Sans ça le beam cède
systématiquement la meilleure case pour aller sur la quatrième, alors que
partager (zéro pour lui, zéro pour nous) vaut mieux que « il prend la
meilleure, on prend la quatrième ».

**Ce que ça coûte : rien.** Son tour est posé une fois par état, avant nos
combinaisons, donc il est déjà dans le plateau de référence du filtre de
revenu. 12 000 plateaux notés par tour, profondeur 26.

**Ce qu'il faut savoir** : la prédiction touche **56 %** des rails qu'il pose
vraiment, et **36 %** de ses encrages. C'est très au-dessus du hasard, mais
traité comme une certitude — donc le beam esquive parfois une case ou une
région pour rien. Deux réglages bornent la casse, `FOE_PREDICT_FROM_DEPTH` et
`FOE_DISRUPT_FROM_DEPTH` : les mettre au-delà de `MAX_BEAM_DEPTH` éteint
l'adversaire et redonne v5.0.

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

<!-- 
- Résultats que je ne comprends pas. Comment se fait-il qu'il y a que 8 states à la depth 2 et 20 formed ? Alors qu'on a BEAM_WIDTH = 48 et un pruning width > 20. La map n'a plus beaucoup de cases mais bien pleins de 20 cases sont adjacent à des rails. Peut être pas aux chemins a* par contre :
depth  states    formed     kept   scored     A*   solves   sweeps
      1       1         4        4        8     20        0       80
      2       8        20       20       30     60        0      480
      3      22      1028     1028     1542    400     1278     1320
      4      48      1634     1536     2304    600     1116     2880
turn : 7692 us / 30 ms  depth 4  3884 states scored  2686 combos formed  1080 A*  2394 solves  4760 sweeps  1 rails (3 paint)  disrupt -1 -->

Vu qu'on peut traiter enormément de combinaisons, le pruning aggressif et biaisé est maintenant contre productif. Il faudrait diminuer ses décision en faveur de la recherche en largueur/profondeur.
Voici 4 idées pour améliorer ce pruning.
Implémente les 4 idées séparémment en fesant 4 nouvelle version intermédiaire et fais les combattre tous ensemble avec v5.0 et v5.4 dans une nouvelle arène/env. "v5.4_pruning_tests". Lorsqu'il auront 200 games chaqu'un, fais des conclusions.

  1. Pruning: Ne pas addition les valeurs sortante des a*, mais garder la meilleur uniquement.
  2. Moins punir les cases inked. plutot que de multipler directement on pourrait faire varier entre 80% et 100% de la valeur de la case.
  3. Ne pas punir les cases inked et laisser la recherche en profondeur prouver que la pose de certains rails dans des case qui vont être inked est une perte de temps, et donc de score. Mettre la pression en posant les rails jusqu'au bout de la conenction peut être aussi benefique.
  4. Le puning aggressif fait que le bot manque clairement de possibilitées, surtout en fin de partie. Par exmeple les chemins les plus court qui coute beaucoup de peinture ne sont jamais considéré alors qu'ils raporterait des points. Il faudrait peut être faire d'autres A* qui ne prendre pas en compte les coups de peinture, afin de quand même s'orienter par default les chemins les plus court en nombre de cases lorsque les chemins les moins couteux en peinture ont déjà été fait.

- Il faut faire une diffusion des valeurs des cases pour créer plus de combinaison pour les premieres depth. Ajouter 4 moitiés sur un case donne un score plus grand que les cases elles mêmes. Il faut ajouter 1/4 de chaque cases sur ses adjacentes qui étaient à 0 au début.
- **L'ordre d'évaluation des combinaisons, aux niveaux profonds.** Elles sont
  triées par somme de notes, et la note ne prédit pas le revenu — c'est mesuré :
  couper ce classement à la racine coûte 16 points. Aux niveaux profonds on le
  coupe quand même, faute de budget. Trier plutôt par « nombre de cases du
  groupe sur un chemin actif » ferait remonter les coups payants.
- **Mieux prédire l'adversaire**

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

### v5.5

**La carte de valeurs servait périmée un tiers du temps.** En fin de partie,
un état héritait de la carte bleue pour le plateau d'un autre : les chemins
contournaient des régions désencrées depuis longtemps. Mesuré sur 10 000
expansions : 35 % de cartes fausses, ramenées à 0 sans rien coûter.

Duel direct contre v5.4 : 95W-65L et 140 nulles, 59,4 % sur les décisives,
IC95 [51,8 – 67,0], p = 0,018.

Réglages inchangés depuis la v5.4 : seul le cache change.

### v5.4

**Le beam est court et large.** Un balayage de 16 variantes croisant la
profondeur (3 et 4) et `BEAM_WIDTH` (24, 32, 48, 64) sur deux pools de
combinaisons, ~440 parties chacune, donne trois résultats convergents :

- **La profondeur 6 est trop longue.** La v5.3 finit 22e sur 23.
- **La profondeur 3 est trop courte.** Toutes les variantes D3 sont dans la
  moitié basse : le beam épuise son arbre avant l'horloge et le budget libéré
  ne se reconvertit en rien (80 états en fin de partie contre 192 pour v5.3).
  Raccourcir n'aide que tant qu'il reste de quoi élargir.
- **`BEAM_WIDTH=48` est l'optimum**, dans le même ordre b48 > b64 > b32 > b24
  pour les deux pools indépendamment.

Réglages : `BEAM_WIDTH=48`, `MAX_BEAM_DEPTH=4`, `COMBO_PRUNING_WIDTH=200`,
`COMBO_DECAY=2`, `COMBO_FLOOR=50`.

**Retenue sur duels contre v5.3** : 214W-186L (53,5 %) et 216W-184L (54,0 %)
pour les deux meilleures variantes, qui ne diffèrent que par un pool dont
l'arène a montré qu'il ne sépare rien. Prises une par une aucune n'est
significative ; regroupées, 430W-370L sur 800 parties, IC95 [50,3 – 57,2],
p = 0,034. Le gain réel est de l'ordre de **4 points**, pas davantage : le
réglage du pruning seul (largeur du pool, decay) n'a jamais rien séparé, et
c'est `COMBO_FLOOR` et le plafond de profondeur qui portent tout l'effet.

First moment in arena: 301/1545 overall & Silver league

### v5.3

Encourage width exploration (with decay) rather than deep and incertain exploration.
Improve metrics

Last moment in arena: 300/1545 overall & Silver league

First moment in arena: 289/1545 overall & Silver league

### v5.1

**Les deux joueurs sont joués à chaque tour de chaque ligne.** L'adversaire
pose ses rails et son encrage sur la même carte de valeurs, et les rails
simultanés se résolvent en neutres. La valeur des cases devient une différence.

**Retenue sur des tests en direct contre v5.0.** : 123W 96L 0D

Last moment in arena: 296/1545 overall & Silver league

First moment in arena: 296/1545 overall & Silver league

### v5.0

**Beam search sur les tours**, posé sur l'élagage de v4 : le coup joué est le
premier tour de la meilleure ligne à travers les depth

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

Last moment in arena: 301/1526 overall & Silver league

First moment in arena: 301/1526 overall & Silver league

### v4.5

- **Le revenu est recalculé, plus lu.** : 1 point par rail possédé sur le chemin au lieu de 1 par wish
- **Seuls les wishes atteignables par les 3 rails sont recalculés** — 12 ms
  → 426 µs. Le filtre doit inclure la frontière du parcours, sinon il est faux.
- **Horloge lue à chaque état** (avant : 1 sur 64)

Les cases choisies tiennent dans les N meilleures	| Fréquence
3	55,7 %
4	69,6 %
6	77,7 %

Last moment in arena: 304/1526 overall & Silver league

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
