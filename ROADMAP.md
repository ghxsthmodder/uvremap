# Roadmap — uvremap pós-v0.1.2

Estado: Fases 1–4 do README implementadas e verificados (golden 203/203, releases
v0.1.0–v0.1.2 com windows-x64 / linux-x86_64 / linux-aarch64). Prioridades abaixo
por impacto e risco.

## M0 — Correções de exatidão (v0.1.3, curar divergências silenciosas)

1. **Primitive mode strip/fan** — hoje `TRIANGLE_STRIP`/`TRIANGLE_FAN` são lidos
   como lista de triângulos sem aviso (silenciosamente errados). Converter para
   triângulos na carga ou marcar a primitiva como inelegível (saída honesta).
2. **Morph targets** — verificar o que acontece com `morphTargets` (posições
   extras ignoradas?). Decidir: repassar in-tactos no export ou rejeitar com
   motivo explícito; nunca reconstruir UV de alvo com geometria base calçada.
3. **Testes de regressão sintéticos** — unidade de modelo com twins colocalizados
   (bug do doghouse) e strip/fan, para a matriz golden não depender só de assets.

## M1 — Cobertura de extensões (v0.2.0)

1. **KHR_texture_transform com rotação** — substituir o motivo de rejeição por
   suporte real na amostragem (UV de origem rotacionado em torno de offset; o
   baker já aplica offset/scale).
2. **KHR_materials_variants** — hoje torna a primitiva inelegível (Shoe → exit 4).
   Plano: rebakear as texturas de cada variante para o mesmo atlas (slots por
   variante clonados junto do material), preservando o mapeamento de variante.
3. **Draco/meshopt** — detectar primitivas comprimidas; ou decodificar (via
   draco/meshopt quando disponíveis) ou rejeitar com aviso claro e exit 4
   honesto (hoje tinygltf sem extensões de decodificação provavelmente já
   falha na carga — padronizar a mensagem).

## M2 — Atlas multi-página e resoluções não quadradas (v0.3.0)

1. **Multi-página real** — hoje charts que não cabem viram aviso e região não
   mapeada (páginas >0 são descartadas). Alternativas: (a) aceitar N páginas,
   N texturas por atlas e `atlasIndex` real no UV export; (b) manter single-page
   e falhar com exit 5 + sugestões concretas. Decidir (a) — é o que os usuários
   esperam de `--resolution` pequeno.
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
