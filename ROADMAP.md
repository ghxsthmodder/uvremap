# Roadmap — uvremap pós-v0.1.4

Estado: Fases 1–4 do README implementadas e verificados (golden 302/302, releases
v0.1.0–v0.1.9 com windows-x64 / linux-x86_64 / linux-aarch64 / android-arm64).
M0 concluído (v0.1.3); M1 concluído (v0.1.5); M2 concluído (multi-página e
não-quadrado; v0.1.5 + pending); M3 concluído (itens 1–4); M4 itens 1–5
concluídos (item 1 na v0.1.8, itens 2–5 na v0.1.9); M5 em andamento (fecha a
série 0.1.x na v0.1.10). Prioridades abaixo por impacto e risco.

## Política de versionamento

Releases seguem `0.1.x`: incrementa-se o patch a cada entrega (`0.1.3`, `0.1.4`,
… até `0.1.10`). Quando o patch chega a 10, o minor avança e o patch volta a
zero (próxima release após `0.1.10` é `0.2.0`). Os milestones abaixo não
carregam versão própria; são entregues em releases 0.1.x sucessivas até estourar
a série.

## M0 — Correções de exatidão (curar divergências silenciosas) — concluído

Status: itens 1–3 implementados e verificados (unit tests + golden 203/203);
lançados na v0.1.3.

1. **Primitive mode strip/fan** — hoje `TRIANGLE_STRIP`/`TRIANGLE_FAN` são lidos
   como lista de triângulos sem aviso (silenciosamente errados). Converter para
   triângulos na carga ou marcar a primitiva como inelegível (saída honesta).
2. **Morph targets** — verificar o que acontece com `morphTargets` (posições
   extras ignoradas?). Decidir: repassar in-tactos no export ou rejeitar com
   motivo explícito; nunca reconstruir UV de alvo com geometria base calçada.
3. **Testes de regressão sintéticos** — unidade de modelo com twins colocalizados
   (bug do doghouse) e strip/fan, para a matriz golden não depender só de assets.

## M1 — Cobertura de extensões

Status: itens 1 e 3 concluídos e lançados na v0.1.4 (rotação suportada no
bake; draco/meshopt recusados com mensagem clara, exit 3). Itens 2 e 4
concluídos (ver abaixo), a lançar. M1 completo.

1. **KHR_texture_transform com rotação** — substituir o motivo de rejeição por
   suporte real na amostragem (UV de origem rotacionado em torno de offset; o
   baker já aplica offset/scale). ✔ Concluído: ordem da spec
   (`uv' = offset + R·(uv·scale)`), primitivas com rotação voltam a ser
   elegíveis; verificado no modelo de conformance
   TextureTransformMultiTest (agreement 99.8% via check_bake.py).
2. **KHR_materials_variants** — hoje torna a primitiva inelegível (Shoe → exit 4).
   Plano: rebakear as texturas de cada variante para o mesmo atlas (slots por
   variante clonados junto do material), preservando o mapeamento de variante.
   ✔ Concluído: mapeamentos carregados e reescritos no export; cada material
   de variante recebe uma cópia dedicada do atlas (camadas por material no
   bake); modo `material` agrupa por conjunto de candidatos. Shoe regenera
   (exit 0); `check_variants.py` valida os 3 modos de atlas (golden 259/259).
3. **Draco/meshopt** — detectar primitivas comprimidas; ou decodificar (via
   draco/meshopt quando disponíveis) ou rejeitar com aviso claro e exit 4
   honesto (hoje tinygltf sem extensões de decodificação provavelmente já
   falha na carga — padronizar a mensagem). ✔ Concluído: arquivos que exigem
   draco/meshopt são recusados na carga com mensagem clara (exit 3);
   primitivas comprimidas em arquivos mistos geram aviso específico e ficam
   inalteradas.
4. **Dobra fatores de material na textura (`--bake-factors`)** — para materiais
   sem `baseColorTexture`, escrever o `baseColorFactor` (sRGB) nos texels do
   primitivo no atlas e neutralizar o material clonado (fator branco); mesmo
   mecanismo para `metallicRoughnessFactor` (G/B), `emissiveFactor` e
   `occlusionStrength` (R). O preenchimento neutro deixa de ser global e passa
   a ser por material. Observações: o glTF já multiplica fator×textura, então
   o resultado visual atual está correto — a opção serve para fundir várias
   primitivas de cores planas em uma única textura/material por atlas;
   atenção à quantização 8-bit de fatores sutis e sangramento de cor no
   padding/mips entre ilhas.
   ✔ Concluído: `--bake-factors` pinta baseColorFactor (linear→sRGB),
   metallicRoughness (G/B) e emissiveFactor nos texels cobertos da primitiva
   (máscara por bounding-box do triângulo, cobre o padding) e neutraliza os
   fatores nos materiais clonados; `occlusionStrength` fica de fora (sem
   textura, adicionar uma mudaria o sombreado). Padrão de neutralidade segue a
   spec (metallic=1, roughness=1; tinygltf usa os mesmos defaults). Sem a
   flag, comportamento inalterado (fatores preservados). Verificado com modelo
   sintético sem textura (`make_factor_model.py`) nos modos material/scene —
   cores pintadas conferem 100% via check_bake.py (golden 268/268).

## M2 — Atlas multi-página e resoluções não quadradas

1. **Multi-página real** — hoje charts que não cabem viram aviso e região não
   mapeada (páginas >0 são descartadas). Alternativas: (a) aceitar N páginas,
   N texturas por atlas e `atlasIndex` real no UV export; (b) manter single-page
   e falhar com exit 5 + sugestões concretas. Decidir (a) — é o que os usuários
   esperam de `--resolution` pequeno.
   ✔ Concluído: empilhamento de páginas. O runner aceita `atlasCount > 1`
   (fim do retry por densidade); o baker rasteriza por página e compos­ta as
   camadas verticalmente numa textura W x (H*N); o UV de saída é
   `v = (page + v_local/H)/N`, então `gltf_io` não muda. Camadas são alocadas
   sob demanda (página vazia não emite nada) e o preview desenha o offset de
   página. Golden: DamagedHelmet 1024x1024 com `--texels-per-unit 400` → 6 páginas,
   94% de concordância via check_bake.py; validado também com doghouse.glb
   privado (8 páginas, exit 0).
2. **Resolução não quadrada** — xatlas só empacota quadrado; avaliar pós-processo
   de renormalização para WxH arbitrário (re-escalonar UVs do atlas) em vez do
   atual `max(W,H)` com aviso.
   ✔ Concluído: o runner re-escala os pixels do pack quadrado para o W x H
   pedido (por eixo) e `gen.width/height` passam a refletir o retângulo real —
   baker, export e preview derivam tudo desses campos. A densidade default é
   dimensionada por sqrt(W*H) (não mais o lado máximo) e o padding do pack é
   ampliado pelo fator de compressão, preservando o gutter em texels no eixo
   comprimido. Golden: DamagedHelmet 2048x1536 → páginas 2048 x k*1536, 93%
   de concordância (golden 273/273).

## M3 — Desempenho e diagnóstico (contínuo)

1. **Paralelismo por atlas** — generation/baking/export por grupo em threads
   (bake atual é single-thread por texel; BoomBox/doghouse pequenos, mas 4K+
   com variantes multiplica).
   ✔ Concluído: baking por grupo em pool de threads (a generation xatlas já é
   internamente threadada; export é leve). Relatórios e stats mesclados em ordem
   de grupo — saída byte-idêntica ao caminho sequencial (determinismo verificado
   por hash das texturas em execuções repetidas, modelo de 28 atlas).
2. **Self-check de bake embutido** — o `check_bake.py` virou referência; portar
   a amostragem estatística para o CLI (`--self-check N`): amostra N pontos por
   atlas, reporta % de concordância no `report.txt`.
   ✔ Concluído: `--self-check N` amostra N texels por imagem reconstruída e
   confere contra a textura de origem (tolerância 40 por canal, como o
   `check_bake.py`; mapas de normal pulados com nota no relatório; concordância
   < 85% gera aviso). Seeds determinísticos; percentuais vão para o `report.txt`
   e para o JSON futuro.
3. **preview melhorado** — lado a lado UV antigo/novo + heatmap de ocupação e
   mapa de densidade de texels.
   ✔ Concluído: `--preview` escreve, por atlas, o layout das ilhas (já
   existia, mas sem flag — agora ligada ao CLI), o comparador lado a lado
   (wireframe das UVs de origem vs. preenchimento do novo atlas) e o par de
   heatmaps (ocupação branco-preto + densidade de texels em escala log,
   azul→vermelho). Páginas empilhadas verticalmente; golden confere as
   dimensões dos PNGs.
4. **JSON do relatório** (`--json`) para integração em pipelines.
   ✔ Concluído: `--json` escreve `report.json` no diretório de saída (tool,
   versão, entrada/saída, modo, stats, avisos, informações, erros e
   `warningCount`) e imprime o mesmo documento na saída padrão em vez do
   resumo legível; strings são escapadas conforme a spec JSON.

## M4 — Consolidação de texturas

1. **Atlas compartilhado: uma imagem por tipo de mapa** (pedido do usuário no
   MF188 — 49 texturas de 2048x4096 para 18 materiais).
   ✔ Concluído: em grupos multi-material (scene/mesh) o baker pinta as cores
   de fator diretamente na camada compartilhada, com banda de 0,5px além das
   arestas do triângulo para fechar costuras sem invadir o vizinho; o layout
   por material só é forçado por `KHR_materials_variants` ou pela nova flag
   `--split-materials`. MF188: 49 imagens → 4 (`scene_0_basecolor.png`,
   `scene_0_metallic_roughness.png`, `scene_0_normal.png`,
   `scene_0_occlusion.png`), 69 → 24 arquivos na saída, self-check 100% em
   basecolor/MR. Unit test `testSharedFactorBake` + checks golden novos
   (shared por padrão, `--split-materials` restaurando `_matN`).

2. **Auditoria `KHR_materials_*`** — as texturas das extensões (clearcoat,
   transmission, sheen, iridescence, specular, anisotropy, volume) são copiadas
   como estão, mas continuam amostrando o conjunto TEXCOORD que o atlas pisa
   quando `--target-uv == --source-uv`: quebra silenciosa.
   ✔ Concluído: a carga coleta os `textureInfo.texCoord` referenciados dentro
   das extensões de material (`Material::extTexCoordSets`); se o conjunto-alvo
   do atlas é um deles, o atlas muda automaticamente para o primeiro conjunto
   livre (o original fica intacto para as extensões) com `info` no relatório.
   Com `--target-uv` explícito não se move nada — emite `aviso`. Unit test
   `testExtTexCoordDetection` + checks golden (TextureTransformMultiTest:
   auto-move, `--target-uv` fixado avisando, sem falso positivo).

3. **Higiene de saída: nada inalcançável** — ao clonar materiais para o atlas,
   os originais ficavam no documento e arrastavam suas texturas (MF188
   variantes: 18 materiais e 23 imagens órfãs, ~130 MB copiados sem uso).
   ✔ Concluído: o export poda materiais, texturas e imagens que nenhuma
   primitiva alcança (inclusive os arquivos que deixam de ser copiados),
   reindexando também os campos dentro de extensões (`KHR_materials_*`
   `index`, `KHR_materials_variants` `material`). MF188 privado: 24 → 4
   arquivos; variante com URIs externas: 143 MB → 18 MB. Novo
   `scripts/verify/check_orphans.py` (7 checks golden + 2 na regressão privada;
   `run_private.sh` agora exige `textures/ == imagens do atlas`). Em GLB com
   imagens já embutidas o chunk binário mantém os bytes antigos (reindexar
   vistas de buffer da geometria ficaria no meio).

4. **`--self-check` sem `--bake-factors` acusa falso positivo** — achado ao
   testar o MF188 de `/root/visual_mods/mf188`: basecolor 2/86 (2,3%) porque o
   verificador espera que o atlas reproduza a aparência completa, enquanto os
   fatores que ficaram no material (não dobrados em textura) não devem ser
   comparados texel a texel.
   ✔ Concluído: o amostrador só compara quando há valor de origem (textura
   decodificada, ou fator dobrado com `--bake-factors`); as amostras sem
   comparação saem da contagem e aparecem na linha do relatório como "N
   sample(s) had no value to compare" — que é justamente o alarme para ilha
   sem pintura (item 5).

5. **Ilha sem cor no atlas compartilhado (`--bake-factors`)** — relatado no
   MF188 de `/root/visual_mods/mf188`: "a cor não foi transferida para o atlas
   em formato de ilha, continua no modelo". Quinze dos dezoito materiais não
   têm `baseColorTexture` (a cor é só o fator). Fatores não neutros eram
   pintados, mas os de fator branco padrão (1,1,1,1) não — e como a imagem
   compartilhada publicava os bits *unidos* de todos os materiais, o export
   atribuía o atlas a eles e neutralizava o fator mesmo sem tinta: ilhas
   pretas sobre o preenchimento neutro, com 5% do atlas pintado.
   ✔ Concluído: basecolor e metallic-roughness pintam também o valor padrão da
   spec, então toda ilha carrega a aparência do seu material; a imagem
   compartilhada lista em `BakedImage::factorMaterials` os materiais cujo fator
   ela realmente contém e o export só atribui o slot a quem foi pintado (os
   bits unidos deixam de decidir). MF188: cobertura do basecolor 5% → 35%,
   buracos por material < 0,2% (sobram só os alphas das texturas de `cam` e
   `ibreler`), self-check basecolor 399/400. Unit test `testNeutralIslandBake`,
   guardas no contador de amostras sem comparação (golden 302/302, privado
   8/8) e terceiro material sem fator nenhum em `make_factor_model.py`.

## M5 — Exatidão e diagnóstico de fechamento (v0.1.10)

Escopo acordado para encerrar a série `0.1.x`; `KHR_texture_basisu` e mipmaps
ficam para a `0.2.x` (ver M6).

1. **Wrap do sampler ignorado no bake** — achado ao investigar a divergência de
   oclusão do MF188 (self-check 76%): `sampleBytes` era sempre chamada com
   `repeat=true`, mas quase todos os modelos declaram `CLAMP_TO_EDGE`
   (10497) — inclusive toda a banca Khronos com mapas de oclusão. Modelos que
   estendem as UVs além de 1,0 (MF188 chega a 5,4) amostravam o lado oposto da
   textura de origem.
   ✔ Concluído: `TextureRef` carrega `wrapS`/`wrapT` resolvidos na carga
   (`readTextureSlot`): sampler declarado → código do JSON, com padrão
   `CLAMP_TO_EDGE`; textura sem sampler → `REPEAT`, como diz a spec. O bake e o
   `--self-check` amostram pelo modo declarado e `sampleBytes` implementa as
   três modalidades (inclusive `MIRRORED_REPEAT`) no nearest e no bilinear.
   MF188: oclusão 76% → 97,4%, basecolor 99,9%, metallic-roughness 100%.
   Unit tests `testSamplerWrapLoad` (carga), `testSampling` (nearest/bilinear ×
   clamp/repeat/mirror) e `testClampWrapBake` (ilha com UV 0..2: com clamp o
   atlas repete a coluna de borda, com repeat mosaica a textura).
   Golden: o `DamagedHelmet` tem UVs até 1,9986 em V e declara um `sampler`
   vazio, que pela spec é `CLAMP_TO_EDGE` — o bake então deixou de concordar
   com o `check_bake.py`, que amostrava a origem sempre em `REPEAT` (`uv % 1`).
   Era o verificador que estava errado, não o bake: seguindo o sampler declarado
   as duas marcas sobem de 94%/93% (M2.1/M2.2) para 95,9%/95,5%. O verificador
   agora resolve `wrapS`/`wrapT` do modelo de entrada (mesmos padrões da spec)
   e continua em `REPEAT` quando o modelo não declara sampler. Golden 302/302,
   privado 8/8, unitários passando.
2. **`--self-check` media errado** — as sobras depois do wrap não eram defeito
   de bake: a amostragem escolhia triângulos com distribuição uniforme, dando o
   mesmo peso a triângulos que colapsam a frações de texel no atlas — nesses a
   leitura cai no gutter neutro e nunca pode concordar — e comparava o texel do
   atlas contra a origem no ponto aleatório, quando o bake grava uma amostra do
   **centro** do texel. Uma ilha menor que a sua textura de origem era, por
   construção, rejeitada (no MF188, `vida`: AO de 512x512 em ~180 texels).
   ✔ Concluído: escolha ponderada pela área no atlas (`std::discrete_distribution`)
   e expectativa relida no centro do texel amostrado, via baricêntricas do ponto
   de destino. Verificado por rasterização densa: 0,1% do footprint de `govde`
   sobrava em preenchimento neutro, ou seja, o bake não tinha buracos — a métrica
   é que estava errada.
3. **Ocupação e densidade de texels no relatório** (`report.txt` + `--json`).
   ✔ Concluído: `measureAtlasUsage()` soma a área no atlas de cada triângulo
   empacotado (descartando os que o xatlas deixou fora de qualquer página e os
   degenerados) e publica, por atlas, `coverage` (fraction do quadro ocupada),
   `texelsPerUnit` (`sqrt(área_em_texels / área_3D)`, comparável com o pedido),
   `surfaceArea` e as páginas. Sai na seção *Atlas usage* do `report.txt` e no
   vetor `atlasUsage` do `report.json`; o `absorb()` do `Report` não mistura as
   entradas porque a medição é feita no laço sequencial de geração, em ordem de
   grupo. Confiabilidade medida: `DamagedHelmet` com `--texels-per-unit 400`
   reporta 402,2 e `--resolution` fixa 2048 reporta 383,0 (dois atlas de 1024
   fariam o mesmo por página). Unit tests `testAtlasUsage` (quadriculado
   sintético: 25% de ocupação, 50 texels/unidade, vértice sem página não conta)
   e o `testRenderJson` estendido; golden novo: `json: atlasUsage occupancy +
   density` e `report.txt: atlas usage line`.
4. **`--fail-under N`** — exit code quando a concordância do self-check cai
   abaixo do piso.
   ✔ Concluído: `selfCheckGroup` registra em `Report::worstAgreement` a menor
   concordância medida (apenas de imagens que realmente amostraram), e o
   `runRegenerate` compara com o piso antes de exportar. Abaixo do piso: exit
   `5` (falha de transferência), nada é publicado e a staging é descartada — o
   gate é uma recusa, não um aviso, que é o que um pipeline precisa. Sozinho o
   piso não mede nada: exige `--self-check N` (exit `2` sem ele, idem para valor
   fora de `(0,100]`). Quando o piso é atendido a linha entra no `report.txt`
   como `self-check: worst agreement X%, floor Y% (floor met)`. Reaproveitar o
   número que o relatório imprime (e não um segundo cálculo) é o ponto: unit
   test `testSelfCheck` extraído confere que `worstAgreement` é exatamente a
   porcentagem publicada (a 4x4 de origem ampliada dá 98,5% por ruído de último
   bit na fronteira dos texels de origem — real, não defeito do bake). Golden:
   `fail-under: breach exits 5`, `breach publishes nothing`, `floor met exits 0`,
   `floor recorded in report`, e os dois casos de argumento inválido.
5. **`--jobs N`** — tamanho do pool de threads do bake.
6. **Banda anti-costura em ilhas texturizadas.**
7. **Modo lote** — vários arquivos de entrada.

## M6 — Texturas comprimidas e mipmaps (0.2.x, adiado da 0.1.10)

1. **`KHR_texture_basisu`** — hoje um arquivo que exige KTX2/Basis é tratado
   como os de draco/meshopt (recusado na carga, exit 3). Para a 0.2.x: transcodificar
   (basisu/`toktx` embutido) e, no export, reempacotar as imagens do atlas em
   KTX2 com os mesmos canais, preservando `sampler`/`texCoord`.
2. **Mipmaps** — as imagens do atlas saem sem cadeia de níveis; `TextureSettingsTest`
   e afins declaram `minFilter` mipados, então o resultado visual muda em superfícies
   minificadas (o mesmo regime que produz a divergência medida no item M5.2).
   Decidir: gerar mipos no export (filtro glTF: box por nível, com o gutter de
   padding) ou documentar a limitação e avisar quando o `minFilter` do modelo pede mip.

## Infra recorrente (a cada release)

- Fluxo já automatizado: bump `project()` → tag `vX.Y.Z` → CI publica 3 binários.
- Manter README atualizado para novas opções (fonte de verdade das fases).
- Amostras privadas em `samples/private/` (gitignored) fazem parte da verificação
  manual de cada milestone; `scripts/verify/run_private.sh` roda a regressão do
  MF188 (scene/gltf + bake-factors + self-check + checagem M4.1) fora do golden.

## Não fazer (decisão consciente)

- Suporte a formatos além de glTF/GLB (o README limita o escopo a glTF).
- Edição de materiais/geometry além da remoção de UV original quando
  `--target-uv == --source-uv` (política não-destrutiva já documentada).
