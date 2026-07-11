-- ============================================================
-- Queries dos Dashboards - Projeto Bolão / Torneira Inteligente
-- ============================================================

-- Taxa de acerto geral
SELECT
  time_bucket('1 day', created_at) AS time,
  ROUND(
    100.0 * COUNT(*) FILTER (WHERE acertou) / NULLIF(COUNT(*) FILTER (WHERE acertou IS NOT NULL), 0),
    1
  ) AS "Taxa de Acerto Geral (%)"
FROM bets
WHERE $__timeFilter(created_at)
GROUP BY time
ORDER BY time ASC;

-- Taxa de acerto por jogo
SELECT
  (tm.nome || ' x ' || tv.nome) AS "Partida",
  ROUND(
    100.0 * COUNT(*) FILTER (WHERE b.acertou) / NULLIF(COUNT(*) FILTER (WHERE b.acertou IS NOT NULL), 0),
    1
  ) AS "Taxa de Acerto (%)"
FROM partidas p
JOIN times tm ON tm.id_time = p.mandante
JOIN times tv ON tv.id_time = p.visitante
JOIN bets b ON b.id_partida = p.id
WHERE b.acertou IS NOT NULL
  AND p.id IN ($Jogo)
GROUP BY p.id, tm.nome, tv.nome
ORDER BY p.id;

-- Quantidade acumulada por cliente
SELECT
  c.nome_completo AS "Cliente",
  COUNT(*) FILTER (WHERE b.acertou) AS "Acertos",
  COALESCE(v.bonus_total, 0) AS "Saldo Acumulado (R$)"
FROM clientes c
JOIN bets b ON b.id_cliente = c.id
LEFT JOIN vinculo_copo_cliente v ON v.id_cliente = c.id
WHERE c.id IN ($Cliente)
GROUP BY c.id, c.nome_completo, v.bonus_total
ORDER BY "Acertos" DESC;

-- Total de apostas em cada resultado
SELECT
  (tm.nome || ' ' || COALESCE(p.gols_mandante::text, '-') || ' x ' || COALESCE(p.gols_visitante::text, '-') || ' ' || tv.nome) AS "Partida",
  p.data AS "Data",
  p.status AS "Status",
  COUNT(*) FILTER (WHERE b.id_time_apostado = p.mandante) AS "Aposta no Mandante",
  COUNT(*) FILTER (WHERE b.id_time_apostado IS NULL) AS "Aposta no Empate",
  COUNT(*) FILTER (WHERE b.id_time_apostado = p.visitante) AS "Aposta no Visitante"
FROM partidas p
JOIN times tm ON tm.id_time = p.mandante
JOIN times tv ON tv.id_time = p.visitante
LEFT JOIN bets b ON b.id_partida = p.id
WHERE p.id IN ($Jogo)
GROUP BY p.id, tm.nome, tv.nome, p.data, p.status, p.gols_mandante, p.gols_visitante
ORDER BY p.id DESC;

-- Quantidade Apostada
SELECT
  (tm.nome || ' x ' || tv.nome) AS "Partida",
  COALESCE(SUM(b.valor_apostado) FILTER (WHERE b.id_time_apostado = p.mandante), 0) AS "Total apostado no Mandante",
  COALESCE(SUM(b.valor_apostado) FILTER (WHERE b.id_time_apostado IS NULL), 0) AS "Total apostado no Empate",
  COALESCE(SUM(b.valor_apostado) FILTER (WHERE b.id_time_apostado = p.visitante), 0) AS "Total apostado no Visitante",
  COALESCE(SUM(b.valor_apostado), 0) AS "Total apostado"
FROM partidas p
JOIN times tm ON tm.id_time = p.mandante
JOIN times tv ON tv.id_time = p.visitante
LEFT JOIN bets b ON b.id_partida = p.id
WHERE p.id IN ($Jogo)
GROUP BY p.id, tm.nome, tv.nome
ORDER BY p.id;

-- Ranking de Clientes por Bônus acumulado
SELECT
  c.nome_completo AS "Cliente",
  v.bonus_total AS "Saldo Acumulado (R$)"
FROM clientes c
JOIN vinculo_copo_cliente v ON v.id_cliente = c.id
ORDER BY v.bonus_total DESC
LIMIT 5;

-- Ranking por acerto
SELECT
  c.nome_completo AS "Cliente",
  COUNT(*) FILTER (WHERE b.acertou) AS "Total de Acertos"
FROM clientes c
JOIN bets b ON b.id_cliente = c.id
GROUP BY c.id, c.nome_completo
ORDER BY "Total de Acertos" DESC
LIMIT 5;
