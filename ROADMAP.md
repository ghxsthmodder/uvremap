# Roadmap — uvremap pós-v0.1.4

Estado: Fases 1–4 do README implementadas e verificados (golden 271/271, releases
v0.1.0–v0.1.4 com windows-x64 / linux-x86_64 / linux-aarch64). M0 concluído
(v0.1.3); M1 concluído (2 e 4 a lançar); M2.1 concluído (multi-página por
empilhamento + BIN chunk real no .glb, a lançar). Prioridades abaixo
por impacto e risco.

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

## M3 — Desempenho e diagnóstico (contínuo)

1. **Paralelismo por atlas** — generation/baking/export por grupo em threads
   (bake atual é single-thread por texel; BoomBox/doghouse pequenos, mas 4K+
   com variantes multiplica).
2. **Self-check de bake embutido** — o `check_bake.py` virou referência; portar
   a amostragem estatística para o CLI (`--self-check N`): amostra N pontos por
   atlas, reporta % de concordância no `report.txt`.
3. **preview melhorado** — lado a lado UV antigo/novo + heatmap de ocupação e
   mapa de densidade de texels.
4. **JSON do relatório** (`--json`) para integração em pipelines.

## Infra recorrente (a cada release)

- Fluxo já automatizado: bump `project()` → tag `vX.Y.Z` → CI publica 3 binários.
- Manter README atualizado para novas opções (fonte de verdade das fases).
- Amostras privadas em `samples/private/` (gitignored) fazem parte da verificação
  manual de cada milestone; considerar script `run_private.sh` separado do golden.

## Não fazer (decisão consciente)

- Suporte a formatos além de glTF/GLB (o README limita o escopo a glTF).
- Edição de materiais/geometry além da remoção de UV original quando
  `--target-uv == --source-uv` (política não-destrutiva já documentada).
